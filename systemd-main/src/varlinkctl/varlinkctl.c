/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>

#include "sd-varlink.h"

#include "build.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-table.h"
#include "io-util.h"
#include "main-func.h"
#include "pager.h"
#include "parse-argument.h"
#include "path-util.h"
#include "pretty-print.h"
#include "terminal-util.h"
#include "varlink-idl-util.h"
#include "varlink-util.h"
#include "verbs.h"
#include "version.h"

static sd_json_format_flags_t arg_json_format_flags = SD_JSON_FORMAT_OFF;
static PagerFlags arg_pager_flags = 0;
static sd_varlink_method_flags_t arg_method_flags = 0;
static bool arg_collect = false;
static bool arg_quiet = false;
static char **arg_graceful = NULL;
static usec_t arg_timeout = 0;

STATIC_DESTRUCTOR_REGISTER(arg_graceful, strv_freep);

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("varlinkctl", "1", &link);
        if (r < 0)
                return log_oom();

        pager_open(arg_pager_flags);

        printf("%1$s [OPTIONS...] COMMAND ...\n\n"
               "%5$sIntrospect Varlink Services.%6$s\n"
               "\n%3$sCommands:%4$s\n"
               "  info ADDRESS           Show service information\n"
               "  list-interfaces ADDRESS\n"
               "                         List interfaces implemented by service\n"
               "  list-methods ADDRESS [INTERFACE…]\n"
               "                         List methods implemented by services or specific\n"
               "                         interfaces\n"
               "  introspect ADDRESS [INTERFACE…]\n"
               "                         Show interface definition\n"
               "  call ADDRESS METHOD [PARAMS]\n"
               "                         Invoke method\n"
               "  validate-idl [FILE]    Validate interface description\n"
               "  help                   Show this help\n"
               "\n%3$sOptions:%4$s\n"
               "  -h --help              Show this help\n"
               "     --version           Show package version\n"
               "     --no-pager          Do not pipe output into a pager\n"
               "     --more              Request multiple responses\n"
               "     --collect           Collect multiple responses in a JSON array\n"
               "     --oneway            Do not request response\n"
               "     --json=MODE         Output as JSON\n"
               "  -j                     Same as --json=pretty on tty, --json=short otherwise\n"
               "  -q --quiet             Do not output method reply\n"
               "     --graceful=ERROR    Treat specified Varlink error as success\n"
               "     --timeout=SECS      Maximum time to wait for method call completion\n"
               "  -E                     Short for --more --timeout=infinity\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal());

        return 0;
}

static int verb_help(int argc, char **argv, void *userdata) {
        return help();
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_MORE,
                ARG_ONEWAY,
                ARG_JSON,
                ARG_COLLECT,
                ARG_GRACEFUL,
                ARG_TIMEOUT,
        };

        static const struct option options[] = {
                { "help",     no_argument,       NULL, 'h'          },
                { "version",  no_argument,       NULL, ARG_VERSION  },
                { "no-pager", no_argument,       NULL, ARG_NO_PAGER },
                { "more",     no_argument,       NULL, ARG_MORE     },
                { "oneway",   no_argument,       NULL, ARG_ONEWAY   },
                { "json",     required_argument, NULL, ARG_JSON     },
                { "collect",  no_argument,       NULL, ARG_COLLECT  },
                { "quiet",    no_argument,       NULL, 'q'          },
                { "graceful", required_argument, NULL, ARG_GRACEFUL },
                { "timeout",  required_argument, NULL, ARG_TIMEOUT  },
                {},
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hjqE", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case 'E':
                        arg_timeout = USEC_INFINITY;
                        _fallthrough_;

                case ARG_MORE:
                        arg_method_flags = (arg_method_flags & ~SD_VARLINK_METHOD_ONEWAY) | SD_VARLINK_METHOD_MORE;
                        break;

                case ARG_ONEWAY:
                        arg_method_flags = (arg_method_flags & ~SD_VARLINK_METHOD_MORE) | SD_VARLINK_METHOD_ONEWAY;
                        break;

                case ARG_COLLECT:
                        arg_collect = true;
                        break;

                case ARG_JSON:
                        r = parse_json_argument(optarg, &arg_json_format_flags);
                        if (r <= 0)
                                return r;

                        break;

                case 'j':
                        arg_json_format_flags = SD_JSON_FORMAT_PRETTY_AUTO|SD_JSON_FORMAT_COLOR_AUTO;
                        break;

                case 'q':
                        arg_quiet = true;
                        break;

                case ARG_GRACEFUL:
                        r = varlink_idl_qualified_symbol_name_is_valid(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to validate Varlink error name '%s': %m", optarg);
                        if (r == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Not a valid Varlink error name: %s", optarg);

                        if (strv_extend(&arg_graceful, optarg) < 0)
                                return log_oom();

                        break;

                case ARG_TIMEOUT:
                        if (isempty(optarg)) {
                                arg_timeout = USEC_INFINITY;
                                break;
                        }

                        r = parse_sec(optarg, &arg_timeout);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --timeout= parameter '%s': %m", optarg);

                        if (arg_timeout == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Timeout cannot be zero.");

                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        /* If more than one reply is expected, imply JSON-SEQ output */
        if (FLAGS_SET(arg_method_flags, SD_VARLINK_METHOD_MORE))
                arg_json_format_flags |= SD_JSON_FORMAT_SEQ;

        strv_sort_uniq(arg_graceful);

        return 1;
}

static int varlink_connect_auto(sd_varlink **ret, const char *where) {
        int r;

        assert(ret);
        assert(where);

        _cleanup_(sd_varlink_unrefp) sd_varlink *vl = NULL;

        if (STARTSWITH_SET(where, "/", "./")) { /* If the string starts with a slash or dot slash we use it as a file system path */
                _cleanup_close_ int fd = -EBADF;
                struct stat st;

                fd = open(where, O_PATH|O_CLOEXEC);
                if (fd < 0)
                        return log_error_errno(errno, "Failed to open '%s': %m", where);

                if (fstat(fd, &st) < 0)
                        return log_error_errno(errno, "Failed to stat '%s': %m", where);

                if (S_ISSOCK(st.st_mode)) {
                        /* Is this a socket in the fs? Then connect() to it. */

                        r = sd_varlink_connect_address(&vl, FORMAT_PROC_FD_PATH(fd));
                        if (r < 0)
                                return log_error_errno(r, "Failed to connect to '%s': %m", where);

                } else if (S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
                        /* Is this an executable binary? Then fork it off. */

                        r = sd_varlink_connect_exec(&vl, where, STRV_MAKE(where)); /* Ideally we'd use FORMAT_PROC_FD_PATH(fd) here too, but that breaks the #! logic */
                        if (r < 0)
                                return log_error_errno(r, "Failed to spawn '%s' process: %m", where);
                } else
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Unrecognized path '%s' is neither an AF_UNIX socket, nor an executable binary.", where);
        } else {
                /* Otherwise assume this is an URL */
                r = sd_varlink_connect_url(&vl, where);
                if (r < 0)
                        return log_error_errno(r, "Failed to connect to URL '%s': %m", where);
        }

        if (arg_timeout != 0) {
                r = sd_varlink_set_relative_timeout(vl, arg_timeout);
                if (r < 0)
                        log_warning_errno(r, "Failed to set Varlink timeout, ignoring: %m");
        }

        *ret = TAKE_PTR(vl);
        return 0;
}

typedef struct GetInfoData {
        const char *vendor;
        const char *product;
        const char *version;
        const char *url;
        char **interfaces;
} GetInfoData;

static void get_info_data_done(GetInfoData *d) {
        assert(d);

        d->interfaces = strv_free(d->interfaces);
}

static int verb_info(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_varlink_unrefp) sd_varlink *vl = NULL;
        const char *url;
        int r;

        assert(argc == 2);
        url = argv[1];

        r = varlink_connect_auto(&vl, url);
        if (r < 0)
                return r;

        sd_json_variant *reply = NULL;
        r = varlink_call_and_log(vl, "org.varlink.service.GetInfo", /* parameters= */ NULL, &reply);
        if (r < 0)
                return r;

        pager_open(arg_pager_flags);

        if (FLAGS_SET(arg_json_format_flags, SD_JSON_FORMAT_OFF)) {
                static const sd_json_dispatch_field dispatch_table[] = {
                        { "vendor",     SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(GetInfoData, vendor),     SD_JSON_MANDATORY },
                        { "product",    SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(GetInfoData, product),    SD_JSON_MANDATORY },
                        { "version",    SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(GetInfoData, version),    SD_JSON_MANDATORY },
                        { "url",        SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(GetInfoData, url),        SD_JSON_MANDATORY },
                        { "interfaces", SD_JSON_VARIANT_ARRAY,  sd_json_dispatch_strv,         offsetof(GetInfoData, interfaces), SD_JSON_MANDATORY },
                        {}
                };
                _cleanup_(get_info_data_done) GetInfoData data = {};

                r = sd_json_dispatch(reply, dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &data);
                if (r < 0)
                        return r;

                strv_sort(data.interfaces);

                if (streq_ptr(argv[0], "list-interfaces")) {
                        STRV_FOREACH(i, data.interfaces)
                                puts(*i);
                } else {
                        _cleanup_(table_unrefp) Table *t = NULL;

                        t = table_new_vertical();
                        if (!t)
                                return log_oom();

                        r = table_add_many(
                                        t,
                                        TABLE_FIELD, "Vendor",
                                        TABLE_STRING, data.vendor,
                                        TABLE_FIELD, "Product",
                                        TABLE_STRING, data.product,
                                        TABLE_FIELD, "Version",
                                        TABLE_STRING, data.version,
                                        TABLE_FIELD, "URL",
                                        TABLE_STRING, data.url,
                                        TABLE_SET_URL, data.url,
                                        TABLE_FIELD, "Interfaces",
                                        TABLE_STRV, data.interfaces);
                        if (r < 0)
                                return table_log_add_error(r);

                        r = table_print(t, NULL);
                        if (r < 0)
                                return table_log_print_error(r);
                }
        } else {
                sd_json_variant *v;

                v = streq_ptr(argv[0], "list-interfaces") ?
                        sd_json_variant_by_key(reply, "interfaces") : reply;

                sd_json_variant_dump(v, arg_json_format_flags, stdout, NULL);
        }

        return 0;
}

typedef struct GetInterfaceDescriptionData {
        const char *description;
} GetInterfaceDescriptionData;

static int verb_introspect(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_varlink_unrefp) sd_varlink *vl = NULL;
        _cleanup_strv_free_ char **auto_interfaces = NULL;
        char **interfaces;
        const char *url;
        bool list_methods;
        int r;

        assert(argc >= 2);
        list_methods = streq(argv[0], "list-methods");
        url = argv[1];
        interfaces = strv_skip(argv, 2);

        STRV_FOREACH(i, interfaces)
                if (!varlink_idl_interface_name_is_valid(*i))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Not a valid Varlink interface name: '%s'", *i);

        r = varlink_connect_auto(&vl, url);
        if (r < 0)
                return r;

        if (strv_isempty(interfaces)) {
                sd_json_variant *reply = NULL;

                /* If no interface is specified, introspect all of them */

                r = varlink_call_and_log(vl, "org.varlink.service.GetInfo", /* parameters= */ NULL, &reply);
                if (r < 0)
                        return r;

                static const sd_json_dispatch_field dispatch_table[] = {
                        { "interfaces", SD_JSON_VARIANT_ARRAY, sd_json_dispatch_strv, 0, SD_JSON_MANDATORY },
                        {}
                };

                r = sd_json_dispatch(reply, dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &auto_interfaces);
                if (r < 0)
                        return r;

                if (strv_isempty(auto_interfaces))
                        return log_error_errno(SYNTHETIC_ERRNO(ENXIO), "Service doesn't report any implemented interfaces.");

                interfaces = strv_sort_uniq(auto_interfaces);
        }

        /* Automatically switch on JSON_SEQ if we output multiple JSON objects */
        if (!list_methods && strv_length(interfaces) > 1)
                arg_json_format_flags |= SD_JSON_FORMAT_SEQ;

        _cleanup_strv_free_ char **methods = NULL;

        STRV_FOREACH(i, interfaces) {
                sd_json_variant *reply = NULL;
                r = varlink_callbo_and_log(
                                vl,
                                "org.varlink.service.GetInterfaceDescription",
                                &reply,
                                SD_JSON_BUILD_PAIR_STRING("interface", *i));
                if (r < 0)
                        return r;

                if (FLAGS_SET(arg_json_format_flags, SD_JSON_FORMAT_OFF) || list_methods) {
                        static const sd_json_dispatch_field dispatch_table[] = {
                                { "description", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, 0, SD_JSON_MANDATORY },
                                {}
                        };
                        _cleanup_(varlink_interface_freep) sd_varlink_interface *vi = NULL;
                        const char *description = NULL;
                        unsigned line = 0, column = 0;

                        r = sd_json_dispatch(reply, dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &description);
                        if (r < 0)
                                return r;

                        if (!list_methods && i > interfaces)
                                print_separator();

                        /* Try to parse the returned description, so that we can add syntax highlighting */
                        r = varlink_idl_parse(ASSERT_PTR(description), &line, &column, &vi);
                        if (r < 0) {
                                if (list_methods)
                                        return log_error_errno(r, "Failed to parse returned interface description at %u:%u: %m", line, column);

                                log_warning_errno(r, "Failed to parse returned interface description at %u:%u, showing raw interface description: %m", line, column);

                                pager_open(arg_pager_flags);
                                fputs_with_newline(stdout, description);
                        } else if (list_methods) {
                                for (const sd_varlink_symbol *const *y = vi->symbols, *symbol; (symbol = *y); y++) {
                                        if (symbol->symbol_type != SD_VARLINK_METHOD)
                                                continue;

                                        r = strv_extendf(&methods, "%s.%s", vi->name, symbol->name);
                                        if (r < 0)
                                                return log_oom();
                                }
                        } else {
                                pager_open(arg_pager_flags);
                                r = sd_varlink_idl_dump(stdout, vi, SD_VARLINK_IDL_FORMAT_COLOR_AUTO, on_tty() ? columns() : SIZE_MAX);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to format parsed interface description: %m");
                        }
                } else {
                        pager_open(arg_pager_flags);
                        sd_json_variant_dump(reply, arg_json_format_flags, stdout, NULL);
                }
        }

        if (list_methods) {
                pager_open(arg_pager_flags);

                strv_sort_uniq(methods);

                if (FLAGS_SET(arg_json_format_flags, SD_JSON_FORMAT_OFF))
                        strv_print(methods);
                else {
                        _cleanup_(sd_json_variant_unrefp) sd_json_variant *j = NULL;

                        r = sd_json_build(&j, SD_JSON_BUILD_STRV(methods));
                        if (r < 0)
                                return log_error_errno(r, "Failed to build JSON array: %m");

                        sd_json_variant_dump(j, arg_json_format_flags, stdout, NULL);
                }
        }

        return 0;
}

static int reply_callback(
                sd_varlink *link,
                sd_json_variant *parameters,
                const char *error,
                sd_varlink_reply_flags_t flags,
                void *userdata)  {

        int *ret = ASSERT_PTR(userdata), r;

        assert(link);

        if (error) {
                /* Propagate the error we received via sd_notify() */
                (void) sd_notifyf(/* unset_environment= */ false, "VARLINKERROR=%s", error);

                if (strv_contains(arg_graceful, error)) {
                        log_full(arg_quiet ? LOG_DEBUG : LOG_INFO,
                                 "Method call returned expected error: %s", error);

                        r = 0;
                } else
                        r = *ret = log_error_errno(SYNTHETIC_ERRNO(EBADE), "Method call failed: %s", error);
        } else
                r = 0;

        if (!arg_quiet)
                sd_json_variant_dump(parameters, arg_json_format_flags, stdout, NULL);

        return r;
}

static int verb_call(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_json_variant_unrefp) sd_json_variant *jp = NULL;
        _cleanup_(sd_varlink_unrefp) sd_varlink *vl = NULL;
        const char *url, *method, *parameter, *source;
        unsigned line = 0, column = 0;
        int r;

        assert(argc >= 3);
        assert(argc <= 4);
        url = argv[1];
        method = argv[2];
        parameter = argc > 3 && !streq(argv[3], "-") ? argv[3] : NULL;

        /* No JSON mode explicitly configured? Then default to the same as -j */
        if (FLAGS_SET(arg_json_format_flags, SD_JSON_FORMAT_OFF))
                arg_json_format_flags = SD_JSON_FORMAT_PRETTY_AUTO|SD_JSON_FORMAT_COLOR_AUTO;

        /* For pipeable text tools it's kinda customary to finish output off in a newline character, and not
         * leave incomplete lines hanging around. */
        arg_json_format_flags |= SD_JSON_FORMAT_NEWLINE;

        if (!varlink_idl_qualified_symbol_name_is_valid(method))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Not a valid qualified method name: '%s' (Expected valid Varlink interface name, followed by a dot, followed by a valid Varlink symbol name.)", method);

        if (parameter) {
                source = "<argv[4]>";

                /* <argv[4]> is correct, as dispatch_verb() shifts arguments by one for the verb. */
                r = sd_json_parse_with_source(parameter, source, 0, &jp, &line, &column);
        } else {
                if (isatty_safe(STDIN_FILENO) && !arg_quiet)
                        log_notice("Expecting method call parameter JSON object on standard input. (Provide empty string or {} for no parameters.)");

                source = "<stdin>";

                r = sd_json_parse_file_at(stdin, AT_FDCWD, source, 0, &jp, &line, &column);
        }
        if (r < 0 && r != -ENODATA)
                return log_error_errno(r, "Failed to parse parameters at %s:%u:%u: %m", source, line, column);

        /* If parsing resulted in ENODATA the provided string was empty. As convenience to users we'll accept
         * that and treat it as equivalent to an empty object: as a call with empty set of parameters. This
         * mirrors how we do this in our C APIs too, where we are happy to accept NULL instead of a proper
         * JsonVariant object for method calls. */

        r = varlink_connect_auto(&vl, url);
        if (r < 0)
                return r;

        if (arg_collect) {
                sd_json_variant *reply = NULL;
                const char *error = NULL;

                r = sd_varlink_collect(vl, method, jp, &reply, &error);
                if (r < 0)
                        return log_error_errno(r, "Failed to issue %s() call: %m", method);
                if (error) {
                        /* Propagate the error we received via sd_notify() */
                        (void) sd_notifyf(/* unset_environment= */ false, "VARLINKERROR=%s", error);

                        if (strv_contains(arg_graceful, error)) {
                                log_full(arg_quiet ? LOG_DEBUG : LOG_INFO,
                                         "Method call %s() returned expected error: %s", method, error);

                                r = 0;
                        } else
                                r = log_error_errno(SYNTHETIC_ERRNO(EBADE), "Method call %s() failed: %s", method, error);
                } else
                        r = 0;

                if (arg_quiet)
                        return r;

                pager_open(arg_pager_flags);
                sd_json_variant_dump(reply, arg_json_format_flags, stdout, NULL);
                return r;

        } else if (arg_method_flags & SD_VARLINK_METHOD_ONEWAY) {
                r = sd_varlink_send(vl, method, jp);
                if (r < 0)
                        return log_error_errno(r, "Failed to issue %s() call: %m", method);

                r = sd_varlink_flush(vl);
                if (r < 0)
                        return log_error_errno(r, "Failed to flush Varlink connection: %m");

        } else if (arg_method_flags & SD_VARLINK_METHOD_MORE) {

                int ret = 0;
                sd_varlink_set_userdata(vl, &ret);

                r = sd_varlink_bind_reply(vl, reply_callback);
                if (r < 0)
                        return log_error_errno(r, "Failed to bind reply callback: %m");

                r = sd_varlink_observe(vl, method, jp);
                if (r < 0)
                        return log_error_errno(r, "Failed to issue %s() call: %m", method);

                for (;;) {
                        r = sd_varlink_is_idle(vl);
                        if (r < 0)
                                return log_error_errno(r, "Failed to check if varlink connection is idle: %m");
                        if (r > 0)
                                break;

                        r = sd_varlink_process(vl);
                        if (r < 0)
                                return log_error_errno(r, "Failed to process varlink connection: %m");
                        if (r != 0)
                                continue;

                        r = sd_varlink_wait(vl, USEC_INFINITY);
                        if (r < 0)
                                return log_error_errno(r, "Failed to wait for varlink connection events: %m");
                }

                return ret;
        } else {
                sd_json_variant *reply = NULL;
                const char *error = NULL;

                r = sd_varlink_call(vl, method, jp, &reply, &error);
                if (r < 0)
                        return log_error_errno(r, "Failed to issue %s() call: %m", method);

                /* If the server returned an error to us, then fail, but first output the associated parameters */
                if (error) {
                        /* Propagate the error we received via sd_notify() */
                        (void) sd_notifyf(/* unset_environment= */ false, "VARLINKERROR=%s", error);

                        if (strv_contains(arg_graceful, error)) {
                                log_full(arg_quiet ? LOG_DEBUG : LOG_INFO,
                                         "Method call %s() returned expected error: %s", method, error);

                                r = 0;
                        } else
                                r = log_error_errno(SYNTHETIC_ERRNO(EBADE), "Method call %s() failed: %s", method, error);
                } else
                        r = 0;

                if (arg_quiet)
                        return r;

                pager_open(arg_pager_flags);

                sd_json_variant_dump(reply, arg_json_format_flags, stdout, NULL);
                return r;
        }

        return 0;
}

static int verb_validate_idl(int argc, char *argv[], void *userdata) {
        _cleanup_(varlink_interface_freep) sd_varlink_interface *vi = NULL;
        _cleanup_free_ char *text = NULL;
        const char *fname;
        unsigned line = 1, column = 1;
        int r;

        fname = argc > 1 ? argv[1] : NULL;

        if (fname) {
                r = read_full_file(fname, &text, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to read interface description file '%s': %m", fname);
        } else {
                r = read_full_stream(stdin, &text, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to read interface description from stdin: %m");

                fname = "<stdin>";
        }

        r = varlink_idl_parse(text, &line, &column, &vi);
        if (r == -EBADMSG)
                return log_error_errno(r, "%s:%u:%u: Bad syntax.", fname, line, column);
        if (r == -ENETUNREACH)
                return log_error_errno(r, "%s:%u:%u: Failed to parse interface description due an unresolved type.", fname, line, column);
        if (r < 0)
                return log_error_errno(r, "%s:%u:%u: Failed to parse interface description: %m", fname, line, column);

        r = varlink_idl_consistent(vi, LOG_ERR);
        if (r == -EUCLEAN)
                return log_error_errno(r, "Interface is inconsistent.");
        if (r == -ENOTUNIQ)
                return log_error_errno(r, "Field or symbol not unique in interface.");
        if (r < 0)
                return log_error_errno(r, "Failed to check interface for consistency: %m");

        if (arg_quiet)
                return 0;

        pager_open(arg_pager_flags);

        r = sd_varlink_idl_dump(stdout, vi, SD_VARLINK_IDL_FORMAT_COLOR_AUTO, on_tty() ? columns() : SIZE_MAX);
        if (r < 0)
                return log_error_errno(r, "Failed to format parsed interface description: %m");

        return 0;
}

static int varlinkctl_main(int argc, char *argv[]) {
        static const Verb verbs[] = {
                { "info",            2,        2,        0, verb_info         },
                { "list-interfaces", 2,        2,        0, verb_info         },
                { "introspect",      2,        VERB_ANY, 0, verb_introspect   },
                { "list-methods",    2,        VERB_ANY, 0, verb_introspect   },
                { "call",            3,        4,        0, verb_call         },
                { "validate-idl",    1,        2,        0, verb_validate_idl },
                { "help",            VERB_ANY, VERB_ANY, 0, verb_help         },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

static int run(int argc, char *argv[]) {
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        return varlinkctl_main(argc, argv);
}

DEFINE_MAIN_FUNCTION(run);