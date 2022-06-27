/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  RFC 7589 Netconf over TLS
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <libgen.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Command line options to be passed to getopt(3) */
#define NETCONF_OPTS "hD:f:E:l:q01ca:u:d:p:y:U:t:eo:"

#define NETCONF_LOGFILE "/tmp/clixon_netconf_tls.log"

/* See see listen(5) */
#define SOCKET_LISTEN_BACKLOG 8

/* Cert verify depth: dont know what to set here? */
#define VERIFY_DEPTH 5

static int             session_id_context = 1;

/*! Ignore errors on packet errors: continue */
static int ignore_packet_errors = 1;

/*!
 * The verify_callback function is used to control the behaviour when the SSL_VERIFY_PEER flag
 * is set. It must be supplied by the application and receives two arguments: preverify_ok
 * indicates, whether the verification of the certificate in question was passed 
 * (preverify_ok=1) or not (preverify_ok=0). x509_ctx is a pointer to the complete context
 * used for the certificate chain verification.
 */
static int
restconf_verify_certs(int             preverify_ok,
		      X509_STORE_CTX *store)
{
    char                buf[256];
    X509               *err_cert;
    int                 err;
    int                 depth;
    //    SSL                *ssl;
    //    clicon_handle       h;
    
    err_cert   = X509_STORE_CTX_get_current_cert(store);
    err        = X509_STORE_CTX_get_error(store);
    depth      = X509_STORE_CTX_get_error_depth(store);
    //    ssl        = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());

    clicon_debug(1, "%s preverify_ok:%d err:%d depth:%d", __FUNCTION__, preverify_ok, err, depth);
    X509_NAME_oneline(X509_get_subject_name(err_cert), buf, 256);
    switch (err){
    case X509_V_ERR_HOSTNAME_MISMATCH:
	clicon_debug(1, "%s X509_V_ERR_HOSTNAME_MISMATCH", __FUNCTION__);
	break;
    case X509_V_ERR_CERT_HAS_EXPIRED:
	clicon_debug(1, "%s X509_V_ERR_CERT_HAS_EXPIRED", __FUNCTION__);
	break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
	clicon_debug(1, "%s X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT", __FUNCTION__);
	break;
    }
    /* Catch a too long certificate chain. should be +1 in SSL_CTX_set_verify_depth() */
    if (depth > VERIFY_DEPTH + 1) {
        preverify_ok = 0;
        err = X509_V_ERR_CERT_CHAIN_TOO_LONG;
        X509_STORE_CTX_set_error(store, err);
    } 
    else{
	/* Verify the CA name */
    }
    //    h = SSL_get_app_data(ssl);
    /* Different schemes for return values if failure detected:
     * - 0 (preferity_ok) the session terminates here in SSL negotiation, an http client
     *     will get a low level error (not http reply)
     * - 1 Check if the cert is valid using SSL_get_verify_result(rc->rc_ssl)
     * @see restconf_nghttp2_sanity where this is done for http/1 and http/2
     */
    return 1;
}

/*! Create and bind restconf socket
 * 
 * @param[in]  netns0    Network namespace, special value "default" is same as NULL
 * @param[in]  addrstr   Address as string, eg "0.0.0.0", "::"
 * @param[in]  addrtype  One of inet:ipv4-address or inet:ipv6-address
 * @param[in]  port      TCP port
 * @param[in]  backlog   Listen backlog, queie of pending connections
 * @param[in]  flags     Socket flags OR:ed in with the socket(2) type parameter
 * @param[out] ss        Server socket (bound for accept)
 */
int
netconf_socket_init(const char   *netns0,
		    const char   *addrstr,
		    const char   *addrtype,
		    uint16_t      port,
		    int           backlog,
		    int           flags,
		    int          *ss)
{
    int                 retval = -1;
    struct sockaddr   * sa;
    struct sockaddr_in6 sin6   = { 0 };
    struct sockaddr_in  sin    = { 0 };
    size_t              sin_len;
    const char         *netns;

    clicon_debug(1, "%s %s %s %s %hu", __FUNCTION__, netns0, addrtype, addrstr, port);
    /* netns default -> NULL */
    if (netns0 != NULL && strcmp(netns0, RESTCONF_NETNS_DEFAULT)==0)
	netns = NULL;
    else
	netns = netns0;
    if (strcmp(addrtype, "inet:ipv6-address") == 0) {
        sin_len          = sizeof(struct sockaddr_in6);
        sin6.sin6_port   = htons(port);
        sin6.sin6_family = AF_INET6;

        inet_pton(AF_INET6, addrstr, &sin6.sin6_addr);
        sa = (struct sockaddr *)&sin6;
    }
    else if (strcmp(addrtype, "inet:ipv4-address") == 0) {
        sin_len             = sizeof(struct sockaddr_in);
        sin.sin_family      = AF_INET;
        sin.sin_port        = htons(port);
        sin.sin_addr.s_addr = inet_addr(addrstr);

        sa = (struct sockaddr *)&sin;
    }
    else{
	clicon_err(OE_XML, EINVAL, "Unexpected addrtype: %s", addrtype);
	return -1;
    }
    if (clixon_netns_socket(netns, sa, sin_len, backlog, flags, addrstr, ss) < 0)
	goto done;
    clicon_debug(1, "%s ss=%d", __FUNCTION__, *ss);
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}

static int
netconf_connection(int   s,
		   void *arg)
{
    int                   retval = -1;

    clicon_debug(1, "%s %d", __FUNCTION__, s);
    retval = 0;
    // done:
    return retval;
} /* netconf_connection */

/*! Accept new socket client
 * @param[in]  fd   Socket (unix or ip)
 * @param[in]  arg  typecast clicon_handle
 * @see openssl_init_socket where this callback is registered
 * @see restconf_accept_client
 * @see nc_accept_tls_session
 */
static int
netconf_accept_client(int   fd,
		      void *arg) 
{
    int      retval = -1;
    SSL_CTX *ctx = (SSL_CTX *)arg;
    SSL     *tls = NULL;
    int      ret;
    int      readmore = 0;
    int      e;

    if ((tls = SSL_new(ctx)) == NULL){
	clicon_err(OE_SSL, 0, "SSL_new");
	goto done;
    }
    SSL_set_fd(tls, fd);
    //    if (((ret = SSL_accept(tls)) == -1) && (SSL_get_error(session->ti.tls, ret) == SSL_ERROR_WANT_READ)) {
    while (readmore){
	readmore = 0;
	if ((ret = SSL_accept(tls)) != 1) {
	    clicon_debug(1, "%s SSL_accept() ret:%d errno:%d", __FUNCTION__, ret, errno);
	    e = SSL_get_error(tls, ret);
	    switch (e){
	    default:
		clicon_err(OE_SSL, 0, "SSL_accept:%d", e);
		goto done;
		break;
	    }
	} /* SSL_accept */
    }
    if (clixon_event_reg_fd(fd, netconf_connection, (void*)NULL, "netconf client socket") < 0)
	goto done;
    retval = 0;
 done:
    clicon_debug(1, "%s retval %d", __FUNCTION__, retval);
    return retval;
} /* netconf_accept_client */

/*! Init openSSL
 * @see restconf_ssl_context_configure
 * @see nc_tls_ctx_set_server_cert_key
 * XXX ctx should be created per session
 */
static int
netconf_tls_init(clicon_handle h)
{
    int        retval = -1;
    SSL_CTX   *ctx; /* SSL context */
    int        ss = -1;
    int        ret;
    /* XXX These are temprary cert files until configs are set */
    //    char      *server_cert_path = "/tmp/srv_cert.pem";
    char      *server_cert_path = "/var/tmp/test_restconf.sh/certs/srv_cert.pem";
    char      *server_key_path = "/var/tmp/test_restconf.sh/certs/srv_key.pem";
    char      *server_ca_cert_path = "/var/tmp/test_restconf.sh/certs/ca_cert.pem";

    clicon_debug(1, "%s", __FUNCTION__);
    if ((ctx = SSL_CTX_new(TLS_server_method())) == NULL) {
	clicon_err(OE_SSL, 0, "SSL_CTX_new");
	goto done;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
		       restconf_verify_certs);
    SSL_CTX_set_verify_depth(ctx, VERIFY_DEPTH+1);
    
    if ((ret = SSL_CTX_load_verify_locations(ctx, server_ca_cert_path, NULL)) != 1){
	clicon_err(OE_SSL, 0, "SSL_CTX_load_verify_locations(%s)", server_ca_cert_path);
	goto done;
    }

    X509_STORE_set_flags(SSL_CTX_get_cert_store(ctx), 0);

    SSL_CTX_set_session_id_context(ctx, (void *)&session_id_context, sizeof(session_id_context));
    SSL_CTX_set_app_data(ctx, h);
    SSL_CTX_set_session_cache_mode(ctx, 0);

    /* Set the key and cert */
    if (SSL_CTX_use_certificate_chain_file(ctx, server_cert_path) != 1) {
        ERR_print_errors_fp(stderr);
	goto done;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx,
				    server_key_path,
				    SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
	goto done;
    }
    if (netconf_socket_init("default", "127.0.0.1", "inet:ipv4-address", 8038,
			    SOCKET_LISTEN_BACKLOG,
			    SOCK_NONBLOCK, /* Also 0 is possible */
			    &ss
			    ) < 0)
	goto done;
    if (clixon_event_reg_fd(ss, netconf_accept_client, ctx, "netconf socket") < 0) 
	goto done;    
    retval = 1;
 done:
    return retval;
}

/*! Clean and close all state of netconf process (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
netconf_terminate(clicon_handle h)
{
    yang_stmt  *yspec;
    cvec       *nsctx;
    cxobj      *x;
    
    /* Delete all plugins, and RPC callbacks */
    clixon_plugin_module_exit(h);
    clicon_rpc_close_session(h);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	ys_free(yspec);
    if ((yspec = clicon_config_yang(h)) != NULL)
	ys_free(yspec);
    if ((nsctx = clicon_nsctx_global_get(h)) != NULL)
	cvec_free(nsctx);
    if ((x = clicon_conf_xml(h)) != NULL)
	xml_free(x);
    xpath_optimize_exit();
    clixon_event_exit();
    clicon_handle_exit(h);
    clixon_err_exit();
    clicon_log_exit();
    return 0;
}

/*! Setup signal handlers
 */
static int
netconf_signal_init (clicon_handle h)
{
    int retval = -1;
    
    if (set_signal(SIGPIPE, SIG_IGN, NULL) < 0){
	clicon_err(OE_UNIX, errno, "Setting DIGPIPE signal");
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

static int
timeout_fn(int s,
	   void *arg)
{
    clicon_err(OE_EVENTS, ETIMEDOUT, "User request timeout");
    return -1; 
}

/*! Usage help routine
 * @param[in]  h      Clixon handle
 * @param[in]  argv0  command line
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
	    "\t-D <level>\tDebug level\n"
    	    "\t-f <file>\tConfiguration file (mandatory)\n"
	    "\t-E <dir> \tExtra configuration file directory\n"
	    "\t-l <s|e|o|n|f<file>> \tLog on (s)yslog, std(e)rr, std(o)ut, (n)one or (f)ile (stderr is default)\n"
            "\t-q\t\tServer does not send hello message on startup\n"
	    "\t-0 \t\tSet netconf base capability to 0, server does not expect hello, force EOM framing\n"
	    "\t-1 \t\tSet netconf base capability to 1, server does not expect hello, force chunked framing\n"
    	    "\t-a UNIX|IPv4|IPv6 Internal backend socket family\n"
    	    "\t-u <path|addr>\tInternal socket domain path or IP addr (see -a)\n"
	    "\t-d <dir>\tSpecify netconf plugin directory dir (default: %s)\n"
	    "\t-p <dir>\tAdd Yang directory path (see CLICON_YANG_DIR)\n"
	    "\t-y <file>\tLoad yang spec file (override yang main module)\n"
	    "\t-U <user>\tOver-ride unix user with a pseudo user for NACM.\n"
	    "\t-t <sec>\tTimeout in seconds. Quit after this time.\n"
	    "\t-e \t\tDont ignore errors on packet input.\n"
	    "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
	    argv0,
	    clicon_netconf_dir(h)
	    );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int              retval = -1;
    int              c;
    char            *argv0 = argv[0];
    int              quiet = 0;
    clicon_handle    h;
    char            *dir;
    int              logdst = CLICON_LOG_STDERR;
    struct passwd   *pw;
    struct timeval   tv = {0,}; /* timeout */
    yang_stmt       *yspec = NULL;
    char            *str;
    uint32_t         id;
    cvec            *nsctx_global = NULL; /* Global namespace context */
    size_t           cligen_buflen;
    size_t           cligen_bufthreshold;
    int              dbg = 0;
    size_t           sz;
    
    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	return -1;
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Set username to clixon handle. Use in all communication to backend */
    if ((pw = getpwuid(getuid())) == NULL){
	clicon_err(OE_UNIX, errno, "getpwuid");
	goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
	goto done;
    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	    usage(h, argv[0]);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv[0]);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	case 'E': /* extra config directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGDIR", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	     break;
	}

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst); 
    clicon_debug_init(dbg, NULL); 
    yang_init(h);
    
    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
	goto done;
    
    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'E': /* extra config dir */
	case 'l':  /* log  */
	    break; /* see above */
	case 'q':  /* quiet: dont write hello */
	    quiet++;
	    break;
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_option_add(h, "CLICON_NETCONF_DIR", optarg) < 0)
		goto done;
	    break;
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'y' : /* Load yang spec file (override yang main module) */
	    if (clicon_option_add(h, "CLICON_YANG_MAIN_FILE", optarg) < 0)
		goto done;
	    break;
	case 'U': /* Clixon 'pseudo' user */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    if (clicon_username_set(h, optarg) < 0)
		goto done;
	    break;
	case 't': /* timeout in seconds */
	    tv.tv_sec = atoi(optarg);
	    break;
	case 'e': /* dont ignore packet errors */
	    ignore_packet_errors = 0;
	    break;
	case '0': /* Force EOM */
	    clicon_option_int_set(h, "CLICON_NETCONF_BASE_CAPABILITY", 0);
	    clicon_option_bool_set(h, "CLICON_NETCONF_HELLO_OPTIONAL", 1);
	    break;
	case '1': /* Hello messages are optional */
	    clicon_option_int_set(h, "CLICON_NETCONF_BASE_CAPABILITY", 1);
	    clicon_option_bool_set(h, "CLICON_NETCONF_HELLO_OPTIONAL", 1);
	    break;
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}
    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    if ((sz = clicon_option_int(h, "CLICON_LOG_STRING_LIMIT")) != 0)
	clicon_log_string_limit_set(sz);

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;

    /* Setup signal handlers, int particular PIPE that occurs if backend closes / restarts */
    if (netconf_signal_init(h) < 0)
	goto done;
    
    /* Initialize plugin module by creating a handle holding plugin and callback lists */
    if (clixon_plugin_module_init(h) < 0)
	goto done;
    /* In case ietf-yang-metadata is loaded by application, handle annotation extension */
    if (yang_metadata_init(h) < 0)
	goto done;    
    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);	

    /* Load netconf plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_netconf_dir(h)) != NULL &&
	clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	goto done;
    
    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;
    /* Add netconf yang spec, used by netconf client and as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	goto done;

    /* Call start function is all plugins before we go interactive */
    if (clixon_plugin_start_all(h) < 0)
	goto done;
#if 1
    /* XXX get session id from backend hello */
    clicon_session_id_set(h, getpid()); 
#endif

    /* Send hello request to backend to get session-id back
     * This is done once at the beginning of the session and then this is
     * used by the client, even though new TCP sessions are created for
     * each message sent to the backend.
     */
    if (clicon_hello_req(h, &id) < 0)
	goto done;
    clicon_session_id_set(h, id);
    
    if (netconf_tls_init(h) < 0)
	goto done;

#ifdef NOTYET    
    /* Send hello to northbound client 
     * Note that this is a violation of RDFC 6241 Sec 8.1:
     * When the NETCONF session is opened, each peer(both client and server) MUST send a <hello..
     */
    if (!quiet){
	if (send_hello(h, 1, id) < 0)
	    goto done;
    }
#ifdef __AFL_HAVE_MANUAL_CONTROL
    /* American fuzzy loop deferred init, see CLICON_NETCONF_HELLO_OPTIONAL=true, see a speedup of x10 */
	__AFL_INIT();
#endif
    if (clixon_event_reg_fd(0, netconf_input_cb, h, "netconf socket") < 0)
	goto done;
#endif
    if (dbg)
	clicon_option_dump(h, dbg);
    if (tv.tv_sec || tv.tv_usec){
	struct timeval t;
	gettimeofday(&t, NULL);
	timeradd(&t, &tv, &t);
	if (clixon_event_reg_timeout(t, timeout_fn, NULL, "timeout") < 0)
	    goto done;
    }

    if (clixon_event_loop(h) < 0)
	goto done;
    retval = 0;
  done:
    if (ignore_packet_errors)
	retval = 0;
    clixon_exit_set(1); /* This is to disable resend mechanism in close-session */
    netconf_terminate(h);
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated", __PROGRAM__, getpid());
    return retval;
}
