#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <glib.h>
#include <string.h>
#include <errno.h>
#include <modbus.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "daemon.h"
#include "chrono.h"
#include "option.h"
#include "keyfile.h"
#include "output.h"

#define BITS_NB 0
#define INPUT_BITS_NB 0
#define REGISTERS_NB 400
#define INPUT_REGISTERS_NB 0

static volatile gboolean stop = FALSE;
static volatile gboolean reload = FALSE;
/* Required to stop server */
static int opt_mode = OPT_MODE_MASTER;
static modbus_t *ctx = NULL;
static int server_socket = 0;

static void sigint_stop(int dummy)
{
    /* Stop the main process */
    stop = TRUE;
    if (opt_mode == OPT_MODE_SLAVE) {
        /* Rude way to stop server */
        modbus_close(ctx);
    } else if (opt_mode == OPT_MODE_SERVER) {
        close(server_socket);
        server_socket = 0;
    }
}

static void sighup_reload(int dummy)
{
    stop = TRUE;
    reload = TRUE;
}

static gboolean collect_listen_output(option_t *opt, modbus_mapping_t *mb_mapping, uint8_t *query, int header_length,
                                      int *output_socket)
{
    /* Write multiple registers and single register */
    if (query[header_length] == 0x10 || query[header_length] == 0x6) {
        int rc;
        int addr = MODBUS_GET_INT16_FROM_INT8(query, header_length + 1);
        int nb = 1;

        if (query[header_length] == 0x10)
            nb = MODBUS_GET_INT16_FROM_INT8(query, header_length + 3);

        /* Write to local unix socket */
        if (opt->verbose)
            g_print("Addr %d: %d values\n", addr, nb);

        if (!output_is_connected(*output_socket))
            *output_socket = output_connect(opt->socket_file, opt->verbose);

        if (output_is_connected(*output_socket)) {
            rc = output_write(*output_socket, -1, NULL, addr, nb, NULL, mb_mapping->tab_registers + addr, opt->verbose);
            if (rc == -1) {
                output_close(*output_socket);
                *output_socket = -1;
            }
        }
        return TRUE;
    } else {
        return FALSE;
    }
}

static int collect_listen_rtu(option_t *opt)
{
    modbus_mapping_t *mb_mapping = NULL;
    uint8_t query[MODBUS_RTU_MAX_ADU_LENGTH];
    int output_socket = 0;
    int header_length = modbus_get_header_length(ctx);

    modbus_set_slave(ctx, opt->id);

    mb_mapping = modbus_mapping_new(BITS_NB, INPUT_BITS_NB, REGISTERS_NB, INPUT_REGISTERS_NB);
    if (mb_mapping == NULL) {
        fprintf(stderr, "Failed to allocate the mapping: %s\n", modbus_strerror(errno));
        return -1;
    }

    while (!stop) {
        int rc;

        rc = modbus_receive(ctx, query);
        if (rc > 0) {
            modbus_reply(ctx, query, rc, mb_mapping);
            collect_listen_output(opt, mb_mapping, query, header_length, &output_socket);
        }
    }

    modbus_mapping_free(mb_mapping);
    output_close(output_socket);

    return 0;
}

static int collect_listen_tcp(option_t *opt)
{
    modbus_mapping_t *mb_mapping = NULL;
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    int output_socket = 0;
    int header_length = modbus_get_header_length(ctx);

    int master_socket;
    fd_set refset;
    fd_set rdset;
    int fdmax;

    mb_mapping = modbus_mapping_new(BITS_NB, INPUT_BITS_NB, REGISTERS_NB, INPUT_REGISTERS_NB);
    if (mb_mapping == NULL) {
        fprintf(stderr, "Failed to allocate the mapping: %s\n", modbus_strerror(errno));
        return -1;
    }

    /* Listen client */
    server_socket = modbus_tcp_listen(ctx, 5);

    /* Clear the reference set of socket */
    FD_ZERO(&refset);
    /* Add the server socket */
    FD_SET(server_socket, &refset);

    /* Keep track of the max file descriptor */
    fdmax = server_socket;

    while (!stop) {
        rdset = refset;
        if (select(fdmax+1, &rdset, NULL, NULL, NULL) == -1) {
            perror("Server select() failure.");
            stop = 1;
            break;
        }

        /* Run through the existing connections looking for data to be read */
        for (master_socket = 0; master_socket <= fdmax; master_socket++) {
            if (!FD_ISSET(master_socket, &rdset)) {
                continue;
            }

            if (master_socket == server_socket) {
                /* A client is asking a new connection */
                socklen_t addrlen;
                struct sockaddr_in clientaddr;
                int newfd;

                /* Handle new connections */
                addrlen = sizeof(clientaddr);
                memset(&clientaddr, 0, sizeof(clientaddr));
                newfd = accept(server_socket, (struct sockaddr *)&clientaddr, &addrlen);
                if (newfd == -1) {
                    perror("Server accept() error");
                } else {
                    FD_SET(newfd, &refset);

                    if (newfd > fdmax) {
                        /* Keep track of the maximum */
                        fdmax = newfd;
                    }
                    if (opt->verbose) {
                        g_print("New connection from %s:%d on socket %d\n",
                                inet_ntoa(clientaddr.sin_addr), clientaddr.sin_port, newfd);
                    }
                }
            } else {
                int rc;

                modbus_set_socket(ctx, master_socket);
                rc = modbus_receive(ctx, query);
                if (rc > 0) {
                    modbus_reply(ctx, query, rc, mb_mapping);
                    collect_listen_output(opt, mb_mapping, query, header_length, &output_socket);
                } else if (rc == -1) {
                    /* This example server in ended on connection closing or
                     * any errors. */
                    if (opt->verbose) {
                        g_print("Connection closed on socket %d\n", master_socket);
                    }
                    close(master_socket);

                    /* Remove from reference set */
                    FD_CLR(master_socket, &refset);

                    if (master_socket == fdmax) {
                        fdmax--;
                    }
                }
            }
        }
    }

    modbus_mapping_free(mb_mapping);
    output_close(output_socket);

    return 0;
}

static void collect_poll(option_t *opt, int nb_server, server_t *servers)
{
    int rc;
    int i;
    int n;
    uint16_t tab_reg[MODBUS_MAX_READ_REGISTERS];
    /* Local unix socket to output */
    int output_socket = 0;

    while (!stop) {
        GTimeVal tv;
        int delta;

        g_get_current_time(&tv);
        /* Seconds until the next multiple of RECORDING_INTERVAL */
        delta = (((tv.tv_sec / opt->interval) + 1) * opt->interval) - tv.tv_sec;
        if (opt->verbose) {
            g_print("Going to sleep for %d seconds...\n", delta);
        }

        rc = usleep(delta * 1000000);
        if (rc == -1) {
            g_warning("usleep has been interrupted\n");
        }

        if (opt->verbose) {
            g_print("Wake up: ");
            print_date_time();
            g_print("\n");
        }

        for (i = 0; i < nb_server; i++) {
            server_t *server = &(servers[i]);

            rc = modbus_set_slave(ctx, server->id);
            if (rc != 0) {
                g_warning("modbus_set_slave with ID %d: %s\n", server->id, modbus_strerror(errno));
                continue;
            }

            for (n = 0; n < server->n; n++) {
                if (opt->verbose) {
                    g_print("ID: %d, addr:%d l:%d\n", server->id, server->addresses[n], server->lengths[n]);
                }

                rc = modbus_read_registers(ctx, server->addresses[n], server->lengths[n], tab_reg);
                if (rc == -1) {
                    g_warning("ID: %d, addr:%d l:%d %s\n", server->id, server->addresses[n], server->lengths[n],
                              modbus_strerror(errno));
                } else {
                    /* Write to local unix socket */
                    if (!output_is_connected(output_socket))
                        output_socket = output_connect(opt->socket_file, opt->verbose);

                    if (output_is_connected(output_socket)) {
                        rc = output_write(output_socket, server->id, server->name, server->addresses[n],
                                          server->lengths[n], server->types[n], tab_reg, opt->verbose);
                        if (rc == -1) {
                            output_close(output_socket);
                            output_socket = -1;
                        }
                    }
                }
            }
        }
    }
    output_close(output_socket);
}

int main(int argc, char *argv[])
{
    int rc = 0;
    option_t *opt = NULL;
    int nb_server = 0;
    server_t *servers = NULL;

reload:

    /* Parse command line options */
    opt = option_new();
    option_parse(opt, argc, argv);

    /* Parse .ini file */
    servers = keyfile_parse(opt, &nb_server);

    if (option_set_undefined(opt) == -1) {
        goto quit;
    }

    /* Launched as daemon */
    if (opt->daemon) {
        pid_t pid = fork();
        if (pid) {
            if (pid < 0) {
                fprintf(stderr, "Failed to fork the main: %s\n", strerror(errno));
            }
            _exit(0);
        }
        pid_file_create(opt->pid_file);
        /* detach from the terminal */
        setsid();
    }

    /* Signal */
    signal(SIGINT, sigint_stop);
    signal(SIGHUP, sighup_reload);

    /* Backend set by default if not defined */
    if (opt->backend == OPT_BACKEND_RTU) {
        ctx = modbus_new_rtu(opt->device, opt->baud, opt->parity[0], opt->data_bit, opt->stop_bit);
    } else {
        /* TCP */
        ctx = modbus_new_tcp(opt->ip, opt->port);
    }

    if (ctx == NULL) {
        fprintf(stderr, "Modbus init error\n");
        rc = -1;
        goto quit;
    }

    modbus_set_debug(ctx, opt->verbose);

    if (opt->mode == OPT_MODE_SLAVE ||
        opt->mode == OPT_MODE_MASTER ||
        opt->mode == OPT_MODE_CLIENT) {
        if (modbus_connect(ctx) == -1)
            goto quit;
    }

    /* Set global var for sigint */
    opt_mode = opt->mode;

    /* Main loop */
    switch (opt->mode) {
        case OPT_MODE_SLAVE:
            if (opt->verbose) {
                g_print("Running in slave mode\n");
            }
            collect_listen_rtu(opt);
            break;
        case OPT_MODE_SERVER:
            if (opt->verbose) {
                g_print("Running in server mode\n");
            }
            collect_listen_tcp(opt);
            break;
        case OPT_MODE_MASTER:
            collect_poll(opt, nb_server, servers);
            break;
        default:
            break;
    }

quit:
    if (opt->daemon) {
        pid_file_delete(opt->pid_file);
    }

    if (opt->backend == OPT_BACKEND_TCP) {
        close(server_socket);
        server_socket = 0;
    }

    modbus_close(ctx);
    modbus_free(ctx);

    keyfile_server_free(nb_server, servers);
    option_free(opt);

    if (reload) {
        stop = FALSE;
        reload = FALSE;
        g_print("Reloading of mbcollect\n");
        goto reload;
    }

    g_print("mbcollect has been stopped.\n");

    return rc;
}
