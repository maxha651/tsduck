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
//  Transport stream processor shared library:
//  Extract PCR's from TS packets.
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsPluginRepository.h"
TSDUCK_SOURCE;

#define DEFAULT_SEPARATOR u";"


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class PCRExtractPlugin: public ProcessorPlugin
    {
    public:
        // Implementation of plugin API
        PCRExtractPlugin(TSP*);
        virtual bool start() override;
        virtual bool stop() override;
        virtual Status processPacket(TSPacket&, bool&, bool&) override;

    private:
        // Description of one PID
        struct PIDContext;
        typedef std::map<PID,PIDContext> PIDContextMap;

        // PCRExtractPlugin private members
        PIDSet        _pids;           // List of PID's to analyze
        UString       _separator;      // Field separator
        bool          _noheader;       // Suppress header
        bool          _good_pts_only;  // Keep "good" PTS only
        bool          _get_pcr;        // Get PCR
        bool          _get_opcr;       // Get OPCR
        bool          _get_pts;        // Get PTS
        bool          _get_dts;        // Get DTS
        bool          _csv_format;     // Output in CSV format
        bool          _log_format;     // Output in log format
        UString       _output_name;    // Output file name (NULL means stderr)
        std::ofstream _output_stream;  // Output stream file
        std::ostream* _output;         // Reference to actual output stream file
        PacketCounter _packet_count;   // Global packets count
        PIDContextMap _stats;          // Per-PID statistics

        // Description of one PID
        struct PIDContext
        {
            PacketCounter packet_count;
            PacketCounter pcr_count;
            PacketCounter opcr_count;
            PacketCounter pts_count;
            PacketCounter dts_count;
            uint64_t      first_pcr;
            uint64_t      first_opcr;
            uint64_t      first_pts;
            uint64_t      last_good_pts;
            uint64_t      first_dts;

            // Constructor
            PIDContext() :
                packet_count(0),
                pcr_count(0),
                opcr_count(0),
                pts_count(0),
                dts_count(0),
                first_pcr(0),
                first_opcr(0),
                first_pts(0),
                last_good_pts(0),
                first_dts(0)
            {
            }
        };

        // Report a value in log format.
        void logValue(const UString& type, PID pid, uint64_t value, uint64_t since_start, uint64_t frequency);

        // Inaccessible operations
        PCRExtractPlugin() = delete;
        PCRExtractPlugin(const PCRExtractPlugin&) = delete;
        PCRExtractPlugin& operator=(const PCRExtractPlugin&) = delete;
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_PROCESSOR(pcrextract, ts::PCRExtractPlugin)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::PCRExtractPlugin::PCRExtractPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"Extracts PCR, OPCR, PTS, DTS from TS packet for analysis", u"[options]"),
    _pids(),
    _separator(),
    _noheader(false),
    _good_pts_only(false),
    _get_pcr(false),
    _get_opcr(false),
    _get_pts(false),
    _get_dts(false),
    _csv_format(false),
    _log_format(false),
    _output_name(),
    _output_stream(),
    _output(0),
    _packet_count(0),
    _stats()
{
    option(u"csv",           'c');
    option(u"dts",           'd');
    option(u"good-pts-only", 'g');
    option(u"log",           'l');
    option(u"noheader",      'n');
    option(u"opcr",           0);
    option(u"output-file",   'o', STRING);
    option(u"pcr",            0);
    option(u"pid",           'p', PIDVAL, 0, UNLIMITED_COUNT);
    option(u"pts",            0);
    option(u"separator",     's', STRING);

    setHelp(u"Options:\n"
            u"\n"
            u"  -c\n"
            u"  --csv\n"
            u"      Report data in CSV (comma-separated values) format. All values are reported\n"
            u"      in decimal. This is the default output format. It is suitable for later\n"
            u"      analysis using tools such as Microsoft Excel.\n"
            u"\n"
            u"  -d\n"
            u"  --dts\n"
            u"      Report Decoding Time Stamps (DTS). By default, if none of --pcr, --opcr,\n"
            u"      --pts, --dts is specified, report them all.\n"
            u"\n"
            u"  -g\n"
            u"  --good-pts-only\n"
            u"      Keep only \"good\" PTS, ie. PTS which have a higher value than the\n"
            u"      previous good PTS. This eliminates PTS from out-of-sequence B-frames.\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
            u"\n"
            u"  -l\n"
            u"  --log\n"
            u"      Report data in \"log\" format through the standard tsp logging system.\n"
            u"      All values are reported in hexadecimal.\n"
            u"\n"
            u"  -n\n"
            u"  --noheader\n"
            u"      Do not output initial header line in CSV format.\n"
            u"\n"
            u"  --opcr\n"
            u"      Report Original Program Clock References (OPCR). By default, if none of\n"
            u"      --pcr, --opcr, --pts, --dts is specified, report them all.\n"
            u"\n"
            u"  -o filename\n"
            u"  --output-file filename\n"
            u"      Output file name for CSV reporting (standard error by default).\n"
            u"\n"
            u"  --pcr\n"
            u"      Report Program Clock References (PCR). By default, if none of --pcr,\n"
            u"      --opcr, --pts, --dts is specified, report them all.\n"
            u"\n"
            u"  -p value\n"
            u"  --pid value\n"
            u"      Specifies a PID to analyze. By default, all PID's are analyzed.\n"
            u"      Several --pid options may be specified.\n"
            u"\n"
            u"  --pts\n"
            u"      Report Presentation Time Stamps (PTS). By default, if none of --pcr,\n"
            u"      --opcr, --pts, --dts is specified, report them all.\n"
            u"\n"
            u"  -s string\n"
            u"  --separator string\n"
            u"      Field separator string in CSV output (default: '" DEFAULT_SEPARATOR u"').\n"
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n");
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::PCRExtractPlugin::start()
{
    getPIDSet(_pids, u"pid", true);
    _separator = value(u"separator", DEFAULT_SEPARATOR);
    _noheader = present(u"noheader");
    _output_name = value(u"output-file");
    _good_pts_only = present(u"good-pts-only");
    _get_pts = present(u"pts");
    _get_dts = present(u"dts");
    _get_pcr = present(u"pcr");
    _get_opcr = present(u"opcr");
    _csv_format = present(u"csv") || !_output_name.empty();
    _log_format = present(u"log");
    if (!_get_pts && !_get_dts && !_get_pcr && !_get_opcr) {
        // Report them all by default
        _get_pts = _get_dts = _get_pcr = _get_opcr = true;
    }
    if (!_csv_format && !_log_format) {
        // Use CSV format by default.
        _csv_format = true;
    }

    // Create the output file if there is one
    if (_output_name.empty()) {
        _output = &std::cerr;
    }
    else {
        _output = &_output_stream;
        _output_stream.open(_output_name.toUTF8().c_str());
        if (!_output_stream) {
            tsp->error(u"cannot create file %s", {_output_name});
            return false;
        }
    }

    // Reset state
    _packet_count = 0;
    _stats.clear();

    // Output header
    if (_csv_format && !_noheader) {
        *_output << "PID" << _separator
                 << "Packet index in TS" << _separator
                 << "Packet index in PID" << _separator
                 << "Type" << _separator
                 << "Count in PID" << _separator
                 << "Value" << _separator
                 << "Value offset in PID" << _separator
                 << "Offset from PCR" << std::endl;
    }
    return true;
}


//----------------------------------------------------------------------------
// Stop method
//----------------------------------------------------------------------------

bool ts::PCRExtractPlugin::stop()
{
    if (!_output_name.empty()) {
        _output_stream.close();
    }
    return true;
}


//----------------------------------------------------------------------------
// Packet processing method
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::PCRExtractPlugin::processPacket(TSPacket& pkt, bool& flush, bool& bitrate_changed)
{
    const PID pid = pkt.getPID();

    // Check if we must analyze this PID.
    if (_pids.test(pid)) {

        PIDContext& pc(_stats[pid]);
        const bool has_pcr = pkt.hasPCR();
        const bool has_opcr = pkt.hasOPCR();
        const bool has_pts = pkt.hasPTS();
        const bool has_dts = pkt.hasDTS();
        const uint64_t pcr = pkt.getPCR();

        if (has_pcr) {
            if (pc.pcr_count++ == 0) {
                pc.first_pcr = pcr;
            }
            if (_get_pcr) {
                if (_csv_format) {
                    *_output << pid << _separator
                             << _packet_count << _separator
                             << pc.packet_count << _separator
                             << "PCR" << _separator
                             << pc.pcr_count << _separator
                             << pcr << _separator
                             << (pcr - pc.first_pcr) << _separator
                             << std::endl;
                }
                logValue(u"PCR", pid, pcr, pcr - pc.first_pcr, SYSTEM_CLOCK_FREQ);
            }
        }

        if (has_opcr) {
            const uint64_t opcr = pkt.getOPCR();
            if (pc.opcr_count++ == 0) {
                pc.first_opcr = opcr;
            }
            if (_get_opcr) {
                if (_csv_format) {
                    *_output << pid << _separator
                             << _packet_count << _separator
                             << pc.packet_count << _separator
                             << "OPCR" << _separator
                             << pc.opcr_count << _separator
                             << opcr << _separator
                             << (opcr - pc.first_opcr) << _separator;
                    if (has_pcr) {
                        *_output << (int64_t(opcr) - int64_t(pcr));
                    }
                    *_output << std::endl;
                }
                logValue(u"OPCR", pid, opcr, opcr - pc.first_opcr, SYSTEM_CLOCK_FREQ);
            }
        }

        if (has_pts) {
            const uint64_t pts = pkt.getPTS();
            if (pc.pts_count++ == 0) {
                pc.first_pts = pc.last_good_pts = pts;
            }
            // Check if this is a "good" PTS, ie. greater than the last good PTS
            // (or wrapping around the max PTS value 2**33)
            const bool good_pts = SequencedPTS (pc.last_good_pts, pts);
            if (good_pts) {
                pc.last_good_pts = pts;
            }
            if (_get_pts && (good_pts || !_good_pts_only)) {
                if (_csv_format) {
                    *_output << pid << _separator
                             << _packet_count << _separator
                             << pc.packet_count << _separator
                             << "PTS" << _separator
                             << pc.pts_count << _separator
                             << pts << _separator
                             << (pts - pc.first_pts) << _separator;
                    if (has_pcr) {
                        *_output << (int64_t(pts) - int64_t(pcr / SYSTEM_CLOCK_SUBFACTOR));
                    }
                    *_output << std::endl;
                }
                logValue(u"PTS", pid, pts, pts - pc.first_pts, SYSTEM_CLOCK_SUBFREQ);
            }
        }

        if (has_dts) {
            const uint64_t dts = pkt.getDTS();
            if (pc.dts_count++ == 0) {
                pc.first_dts = dts;
            }
            if (_get_dts) {
                if (_csv_format) {
                    *_output << pid << _separator
                             << _packet_count << _separator
                             << pc.packet_count << _separator
                             << "DTS" << _separator
                             << pc.dts_count << _separator
                             << dts << _separator
                             << (dts - pc.first_dts) << _separator;
                    if (has_pcr) {
                        *_output << (int64_t (dts) - int64_t (pcr / SYSTEM_CLOCK_SUBFACTOR));
                    }
                    *_output << std::endl;
                }
                logValue(u"DTS", pid, dts, dts - pc.first_dts, SYSTEM_CLOCK_SUBFREQ);
            }
        }

        pc.packet_count++;
    }

    _packet_count++;
    return TSP_OK;
}


//----------------------------------------------------------------------------
// Report a value in log format.
//----------------------------------------------------------------------------

void ts::PCRExtractPlugin::logValue(const UString& type, PID pid, uint64_t value, uint64_t since_start, uint64_t frequency)
{
    if (_log_format) {
        // Number of hexa digits: 11 for PCR (42 bits) and 9 for PTS/DTS (33 bits).
        const size_t width = frequency == SYSTEM_CLOCK_FREQ ? 11 : 9;
        tsp->info(u"PID: 0x%X (%d), %s: 0x%0*X, (0x%0*X, %'d ms from start of PID)",
                  {pid, pid,
                   type, width, value,
                   width, since_start,
                   (since_start * MilliSecPerSec) / frequency});
    }
}
