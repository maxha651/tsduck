//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2018, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Transport stream processor command-line options
//
//----------------------------------------------------------------------------

#include "tspOptions.h"
#include "tsSysUtils.h"
#include "tsAsyncReport.h"
#include "tsPluginRepository.h"
TSDUCK_SOURCE;

#define DEF_BUFSIZE_MB            16  // mega-bytes
#define DEF_BITRATE_INTERVAL       5  // seconds
#define DEF_MAX_FLUSH_PKT_OFL  10000  // packets
#define DEF_MAX_FLUSH_PKT_RT    1000  // packets
#define DEF_MAX_INPUT_PKT_OFL      0  // packets
#define DEF_MAX_INPUT_PKT_RT    1000  // packets

// Displayable names of plugin types.
const ts::Enumeration ts::tsp::Options::PluginTypeNames({
    {u"input",            ts::tsp::Options::INPUT},
    {u"output",           ts::tsp::Options::OUTPUT},
    {u"packet processor", ts::tsp::Options::PROCESSOR},
});

// Options for --list-processor.
//!
const ts::Enumeration ts::tsp::Options::ListProcessorEnum({
    {u"all",    ts::PluginRepository::LIST_ALL},
    {u"input",  ts::PluginRepository::LIST_INPUT  | ts::PluginRepository::LIST_COMPACT},
    {u"output", ts::PluginRepository::LIST_OUTPUT | ts::PluginRepository::LIST_COMPACT},
    {u"packet", ts::PluginRepository::LIST_PACKET | ts::PluginRepository::LIST_COMPACT},
});


//----------------------------------------------------------------------------
// Constructor from command line options
//----------------------------------------------------------------------------

ts::tsp::Options::Options(int argc, char *argv[]) :
    Args(),
    timed_log(false),
    list_proc_flags(0),
    monitor(false),
    ignore_jt(false),
    sync_log(false),
    bufsize(0),
    log_msg_count(AsyncReport::MAX_LOG_MESSAGES),
    max_flush_pkt(0),
    max_input_pkt(0),
    instuff_nullpkt(0),
    instuff_inpkt(0),
    instuff_start(0),
    instuff_stop(0),
    bitrate(0),
    bitrate_adj(0),
    realtime(MAYBE),
    input(),
    output(),
    plugins()
{
    option(u"add-input-stuffing",       'a', STRING);
    option(u"add-start-stuffing",        0,  UNSIGNED);
    option(u"add-stop-stuffing",         0,  UNSIGNED);
    option(u"bitrate",                  'b', POSITIVE);
    option(u"bitrate-adjust-interval",   0,  POSITIVE);
    option(u"buffer-size-mb",            0,  POSITIVE);
    option(u"ignore-joint-termination", 'i');
    option(u"list-processors",          'l', ListProcessorEnum, 0, 1, true);
    option(u"log-message-count",         0,  POSITIVE);
    option(u"max-flushed-packets",       0,  POSITIVE);
    option(u"max-input-packets",         0,  POSITIVE);
    option(u"no-realtime-clock",         0); // was a temporary workaround, now ignored
    option(u"realtime",                 'r', TRISTATE, 0, 1, -255, 256, true);
    option(u"monitor",                  'm');
    option(u"synchronous-log",          's');
    option(u"timed-log",                't');

#if defined(TS_WINDOWS)
#define HELP_SHLIB    u"DLL"
#define HELP_SHLIBS   u"DLL's"
#define HELP_SHLIBEXT u".dll"
#define HELP_SEP      u"'\\'"
#define HELP_SEEMAN   u""
#else
#define HELP_SHLIB    u"shared library"
#define HELP_SHLIBS   u"shared libraries"
#define HELP_SHLIBEXT u".so"
#define HELP_SEP      u"'/'"
#define HELP_SEEMAN   u" See the man page of dlopen(3) for more details."
#endif

    setDescription(u"MPEG transport stream processor using a chain of plugins");

    setSyntax(u"[tsp-options] \\\n"
              u"    [-I input-name [input-options]] \\\n"
              u"    [-P processor-name [processor-options]] ... \\\n"
              u"    [-O output-name [output-options]]");

    setHelp(u"The transport stream processor receives a TS from a user-specified input\n"
            u"plug-in, apply MPEG packet processing through several user-specified packet\n"
            u"processor plug-in's and send the processed stream to a user-specified output\n"
            u"plug-in. All input, processors and output plug-in's are " HELP_SHLIBS u".\n"
            u"\n"
            u"All tsp-options must be placed on the command line before the input,\n"
            u"processors and output specifications. The tsp-options are:\n"
            u"\n"
            u"  -a nullpkt/inpkt\n"
            u"  --add-input-stuffing nullpkt/inpkt\n"
            u"      Specify that <nullpkt> null TS packets must be automatically inserted\n"
            u"      after every <inpkt> input TS packets. Both <nullpkt> and <inpkt> must\n"
            u"      be non-zero integer values. This option is useful to artificially\n"
            u"      increase the input bitrate by adding stuffing. Example: the option\n"
            u"      \"-a 14/24\" adds 14 null packets every 24 input packets, effectively\n"
            u"      turning a 24 Mb/s input stream (terrestrial) into a 38 Mb/s stream\n"
            u"      (satellite).\n"
            u"\n"
            u"  --add-start-stuffing count\n"
            u"      Specify that <count> null TS packets must be automatically inserted\n"
            u"      at the start of the processing, before what comes from the input plugin.\n"
            u"\n"
            u"  --add-stop-stuffing count\n"
            u"      Specify that <count> null TS packets must be automatically inserted\n"
            u"      at the end of the processing, after what comes from the input plugin.\n"
            u"\n"
            u"  -b value\n"
            u"  --bitrate value\n"
            u"      Specify the input bitrate, in bits/seconds. By default, the input\n"
            u"      bitrate is provided by the input plugin or by analysis of the PCR.\n"
            u"\n"
            u"  --bitrate-adjust-interval value\n"
            u"      Specify the interval in seconds between bitrate adjustments,\n"
            u"      ie. when the output bitrate is adjusted to the input one.\n"
            u"      The default is " TS_USTRINGIFY(DEF_BITRATE_INTERVAL) u" seconds.\n"
            u"      Some output processors ignore this setting. Typically, ASI\n"
            u"      or modulator devices use it, while file devices ignore it.\n"
            u"      This option is ignored if --bitrate is specified.\n"
            u"\n"
            u"  --buffer-size-mb value\n"
            u"      Specify the buffer size in mega-bytes. This is the size of\n"
            u"      the buffer between the input and output devices. The default\n"
            u"      is " TS_USTRINGIFY(DEF_BUFSIZE_MB) u" MB.\n"
            u"\n"
            u"  -d[N]\n"
            u"  --debug[=N]\n"
            u"      Produce debug output. Specify an optional debug level N.\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
            u"\n"
            u"  -i\n"
            u"  --ignore-joint-termination\n"
            u"      Ignore all --joint-termination options in plugins.\n"
            u"      The idea behind \"joint termination\" is to terminate tsp when several\n"
            u"      plugins have jointly terminated their processing. Some plugins have\n"
            u"      a --joint-termination option. When set, the plugin executes until some\n"
            u"      plugin-specific condition. When all plugins with --joint-termination set\n"
            u"      have reached their termination condition, tsp terminates. The option\n"
            u"      --ignore-joint-termination disables the termination of tsp when all\n"
            u"      plugins have reached their joint termination condition.\n"
            u"\n"
            u"  -l\n"
            u"  --list-processors\n"
            u"      List all available processors.\n"
            u"\n"
            u"  --log-message-count value\n"
            u"      Specify the maximum number of buffered log messages. Log messages are\n"
            u"      displayed asynchronously in a low priority thread. This value specifies\n"
            u"      the maximum number of buffered log messages in memory, before being\n"
            u"      displayed. When too many messages are logged in a short period of time,\n"
            u"      while plugins use all CPU power, extra messages are dropped. Increase\n"
            u"      this value if you think that too many messages are dropped. The default\n"
            u"      is " + UString::Decimal(AsyncReport::MAX_LOG_MESSAGES) + u" messages.\n"
            u"\n"
            u"  --max-flushed-packets value\n"
            u"      Specify the maximum number of packets to be processed before flushing\n"
            u"      them to the next processor or the output. When the processing time\n"
            u"      is high and some packets are lost, try decreasing this value. The default\n"
            u"      is " + UString::Decimal(DEF_MAX_FLUSH_PKT_OFL) + u" packets in offline mode and " +
            UString::Decimal(DEF_MAX_FLUSH_PKT_RT) + u" in real-time mode.\n"
            u"\n"
            u"  --max-input-packets value\n"
            u"      Specify the maximum number of packets to be received at a time from\n"
            u"      the input plug-in. By default, in offline mode, tsp reads as many packets\n"
            u"      as it can, depending on the free space in the buffer. In real-time mode,\n"
            u"      the default is " + UString::Decimal(DEF_MAX_INPUT_PKT_RT) + u" packets.\n"
            u"\n"
            u"  -m\n"
            u"  --monitor\n"
            u"      Continuously monitor the system resources which are used by tsp.\n"
            u"      This includes CPU load, virtual memory usage. Useful to verify the\n"
            u"      stability of the application.\n"
            u"\n"
            u"  -r[value]\n"
            u"  --realtime[=value]\n"
            u"      Specifies if tsp and all plugins should use default values for real-time\n"
            u"      or offline processing. By default, if any plugin prefers real-time, the\n"
            u"      real-time defaults are used. If no plugin prefers real-time, the offline\n"
            u"      default are used. If -r or --realtime is used alone, the real-time defaults\n"
            u"      are enforced. The explicit values 'no', 'false', 'off' are used to enforce\n"
            u"      the offline defaults and the explicit values 'yes', 'true', 'on' are used\n"
            u"      to enforce the real-time defaults.\n"
            u"\n"
            u"  -s\n"
            u"  --synchronous-log\n"
            u"      Each logged message is guaranteed to be displayed, synchronously, without\n"
            u"      any loss of message. The downside is that a plugin thread may be blocked\n"
            u"      for a short while when too many messages are logged. This option shall be\n"
            u"      used when all log messages are needed and the source and destination are\n"
            u"      not live streams (files for instance). This option is not recommended for\n"
            u"      live streams, when the responsiveness of the application is more important\n"
            u"      than the logged messages.\n"
            u"\n"
            u"  -t\n"
            u"  --timed-log\n"
            u"      Each logged message contains a time stamp.\n"
            u"\n"
            u"  -v\n"
            u"  --verbose\n"
            u"      Produce verbose output.\n"
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n"
            u"\n"
            u"The following options activate the user-specified plug-in's.\n"
            u"\n"
            u"  -I name\n"
            u"  --input name\n"
            u"      Designate the " HELP_SHLIB u" plug-in for packet input.\n"
            u"      By default, read packets from standard input.\n"
            u"\n"
            u"  -O name\n"
            u"  --output name\n"
            u"      Designate the " HELP_SHLIB u" plug-in for packet output.\n"
            u"      By default, write packets to standard output.\n"
            u"\n"
            u"  -P name\n"
            u"  --processor name\n"
            u"      Designate a " HELP_SHLIB u" plug-in for packet processing. Several\n"
            u"      packet processors are allowed. Each packet is successively processed\n"
            u"      by each processor, in the order of the command line. By default, there\n"
            u"      is no processor and the packets are directly passed from the input to\n"
            u"      the output.\n"
            u"\n"
            u"The specified <name> is used to locate a " HELP_SHLIB u". It can be designated\n"
            u"in a number of ways, in the following order:\n"
            u"\n"
            u"  . If the name contains a " HELP_SEP u", it is only interpreted as a file path for\n"
            u"    the " HELP_SHLIB u".\n"
            u"  . If not found, the file is searched into the all directories in environment\n"
            u"    variable " TS_PLUGINS_PATH u" and in the same directory as the tsp executable\n"
            u"    file. In each directory, file named tsplugin_<name>" HELP_SHLIBEXT u" is searched\n"
            u"    first, then the file <name>, with or without " HELP_SHLIBEXT u".\n"
            u"  . Finally, the standard system algorithm is applied to locate the " HELP_SHLIB u"\n"
            u"    file." HELP_SEEMAN u"\n"
            u"\n"
            u"Input-options, processor-options and output-options are specific to their\n"
            u"corresponding plug-in. Try \"tsp {-I|-O|-P} name --help\" to display the\n"
            u"help text for a specific plug-in.\n");

    // Load arguments and process redirections.
    const UString app_name(argc > 0 ? BaseName(UString::FromUTF8(argv[0]), TS_EXECUTABLE_SUFFIX) : UString());
    UStringVector args;
    if (argc > 1) {
        UString::Assign(args, argc - 1, argv + 1);
    }
    if (!processArgsRedirection(args)) {
        exitOnError();
        return;
    }

    // Locate the first processor option. All preceeding options are tsp options and must be analyzed.
    PluginType plugin_type;
    size_t plugin_index = nextProcOpt(args, 0, plugin_type);

    // Analyze the tsp command, not including the plugin options, not processing redirections.
    analyze(app_name, UStringVector(args.begin(), args.begin() + plugin_index), false);

    timed_log = present(u"timed-log");
    list_proc_flags = present(u"list-processors") ? intValue<int>(u"list-processors", PluginRepository::LIST_ALL) : 0;
    monitor = present(u"monitor");
    sync_log = present(u"synchronous-log");
    bufsize = 1024 * 1024 * intValue<size_t>(u"buffer-size-mb", DEF_BUFSIZE_MB);
    bitrate = intValue<BitRate>(u"bitrate", 0);
    bitrate_adj = MilliSecPerSec * intValue(u"bitrate-adjust-interval", DEF_BITRATE_INTERVAL);
    max_flush_pkt = intValue<size_t>(u"max-flushed-packets", 0);
    max_input_pkt = intValue<size_t>(u"max-input-packets", 0);
    instuff_start = intValue<size_t>(u"add-start-stuffing", 0);
    instuff_stop = intValue<size_t>(u"add-stop-stuffing", 0);
    log_msg_count = intValue<size_t>(u"log-message-count", AsyncReport::MAX_LOG_MESSAGES);
    ignore_jt = present(u"ignore-joint-termination");
    realtime = tristateValue(u"realtime");

    if (present(u"add-input-stuffing") && !value(u"add-input-stuffing").scan(u"%d/%d", {&instuff_nullpkt, &instuff_inpkt})) {
        error(u"invalid value for --add-input-stuffing, use \"nullpkt/inpkt\" format");
    }

    // The first processor is always the input.
    // The default input is the standard input file.
    input.type = INPUT;
    input.name = u"file";
    input.args.clear();

    // The default output is the standard output file.
    output.type = OUTPUT;
    output.name = u"file";
    output.args.clear();

    // Locate all plugins
    plugins.reserve(args.size());
    bool got_input = false;
    bool got_output = false;

    while (plugin_index < args.size()) {

        // Check that a plugin name is present after the processor option.
        if (plugin_index >= args.size() - 1) {
            error(u"missing plugin name for option %s", {args[plugin_index]});
            break;
        }

        // Locate plugin description, search for next plugin
        const size_t start = plugin_index;
        PluginType type = plugin_type;
        plugin_index = nextProcOpt(args, plugin_index + 2, plugin_type);
        PluginOptions* opt = 0;

        switch (type) {
            case PROCESSOR:
                plugins.resize(plugins.size() + 1);
                opt = &plugins[plugins.size() - 1];
                break;
            case INPUT:
                if (got_input) {
                    error(u"do not specify more than one input plugin");
                }
                got_input = true;
                opt = &input;
                break;
            case OUTPUT:
                if (got_output) {
                    error(u"do not specify more than one output plugin");
                }
                got_output = true;
                opt = &output;
                break;
            default:
                // Should not get there
                assert(false);
        }

        opt->type = type;
        opt->name = args[start + 1];
        opt->args.clear();
        opt->args.insert(opt->args.begin(), args.begin() + start + 2, args.begin() + plugin_index);
    }

    // Debug display
    if (maxSeverity() >= 2) {
        display(std::cerr);
    }

    // Final checking
    exitOnError();
}


//----------------------------------------------------------------------------
// Apply default values to options which were not specified.
//----------------------------------------------------------------------------

void ts::tsp::Options::applyDefaults(bool rt)
{
    if (max_flush_pkt == 0) {
        max_flush_pkt = rt ? DEF_MAX_FLUSH_PKT_RT : DEF_MAX_FLUSH_PKT_OFL;
    }
    if (max_input_pkt == 0) {
        max_input_pkt = rt ? DEF_MAX_INPUT_PKT_RT: DEF_MAX_INPUT_PKT_OFL;
    }
    debug(u"using --max-input-packets %'d --max-flushed-packets %'d", {max_input_pkt, max_flush_pkt});
}


//----------------------------------------------------------------------------
// Search the next plugin option.
//----------------------------------------------------------------------------

size_t ts::tsp::Options::nextProcOpt(const UStringVector& args, size_t index, PluginType& type)
{
    while (index < args.size()) {
        const UString& arg(args[index]);
        if (arg == u"-I" || arg == u"--input") {
            type = INPUT;
            return index;
        }
        if (arg == u"-O" || arg == u"--output") {
            type = OUTPUT;
            return index;
        }
        if (arg == u"-P" || arg == u"--processor") {
            type = PROCESSOR;
            return index;
        }
        index++;
    }
    return std::min(args.size(), index);
}


//----------------------------------------------------------------------------
// Display the content of the object to a stream
//----------------------------------------------------------------------------

std::ostream& ts::tsp::Options::display(std::ostream& strm, int indent) const
{
    const std::string margin(indent, ' ');
    strm << margin << "* tsp options:" << std::endl
         << margin << "  --add-input-stuffing: " << UString::Decimal(instuff_nullpkt)
         << "/" << UString::Decimal(instuff_inpkt) << std::endl
         << margin << "  --bitrate: " << UString::Decimal(bitrate) << " b/s" << std::endl
         << margin << "  --bitrate-adjust-interval: " << UString::Decimal(bitrate_adj) << " milliseconds" << std::endl
         << margin << "  --buffer-size-mb: " << UString::Decimal(bufsize) << " bytes" << std::endl
         << margin << "  --debug: " << maxSeverity() << std::endl
         << margin << "  --list-processors: " << list_proc_flags << std::endl
         << margin << "  --max-flushed-packets: " << UString::Decimal(max_flush_pkt) << std::endl
         << margin << "  --max-input-packets: " << UString::Decimal(max_input_pkt) << std::endl
         << margin << "  --realtime: " << UString::TristateTrueFalse(realtime) << std::endl
         << margin << "  --monitor: " << monitor << std::endl
         << margin << "  --verbose: " << verbose() << std::endl
         << margin << "  Number of packet processors: " << plugins.size() << std::endl
         << margin << "  Input plugin:" << std::endl;
    input.display(strm, indent + 4);
    for (size_t i = 0; i < plugins.size(); ++i) {
        strm << margin << "  Packet processor plugin " << (i+1) << ":" << std::endl;
        plugins[i].display(strm, indent + 4);
    }
    strm << margin << "  Output plugin:" << std::endl;
    output.display(strm, indent + 4);
    return strm;
}


//----------------------------------------------------------------------------
// Default constructor for plugin options.
//----------------------------------------------------------------------------

ts::tsp::Options::PluginOptions::PluginOptions() :
    type(PROCESSOR),
    name(),
    args()
{
}


//----------------------------------------------------------------------------
// Display the content of the object to a stream
//----------------------------------------------------------------------------

std::ostream& ts::tsp::Options::PluginOptions::display(std::ostream& strm, int indent) const
{
    const std::string margin(indent, ' ');
    strm << margin << "Name: " << name << std::endl
         << margin << "Type: " << PluginTypeNames.name(type) << std::endl;
    for (size_t i = 0; i < args.size(); ++i) {
        strm << margin << "Arg[" << i << "]: \"" << args[i] << "\"" << std::endl;
    }
    return strm;
}
