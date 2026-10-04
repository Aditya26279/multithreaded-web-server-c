/*
 * server_st.c - single-threaded baseline.
 *
 * One thread does everything: accept, read, handle, write, close, repeat.
 * While it is busy with one request (say, waiting 10 ms on "I/O"), every
 * other client waits in the kernel's listen backlog.
 */
#include "common.h"
#include "http.h"
#include "log.h"

int main(int argc, char **argv) {
    options_t opt;
    server_ctx_t ctx;
    sock_t listen_fd;

    parse_options(argc, argv, &opt, 0);
    if (net_init() != 0) return 1;

    if ((listen_fd = net_listen(opt.bind_addr, opt.port)) == INVALID_SOCK) return 1;
    server_ctx_init(&ctx, "single-threaded", opt.docroot, 1, opt.verbose,
                    opt.demo_endpoints, NULL);
    if (opt.verbose && log_start() != 0) return 1;

    printf("[single-threaded] listening on http://%s:%d  (docroot '%s'%s)\n",
           opt.bind_addr, opt.port, opt.docroot,
           opt.demo_endpoints ? "" : ", demo endpoints off");
    printf("press Ctrl+C to stop\n");
    fflush(stdout);

    install_shutdown_handler(listen_fd);
    run_accept_loop(listen_fd, http_handle, &ctx); /* handle inline */

    if (opt.verbose) log_stop();
    printf("\nserved %llu requests\n", ctx.stats.c.requests);
    server_ctx_destroy(&ctx);
    net_cleanup();
    return 0;
}
