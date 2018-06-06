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
//  DVB-CSA or ATIS-IDSA Scrambler
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsPluginRepository.h"
#include "tsServiceDiscovery.h"
#include "tsTSScrambling.h"
#include "tsByteBlock.h"
#include "tsCyclingPacketizer.h"
#include "tsOneShotPacketizer.h"
#include "tsECMGClient.h"
#include "tsBetterSystemRandomGenerator.h"
#include "tsCADescriptor.h"
#include "tsScramblingDescriptor.h"
TSDUCK_SOURCE;

#define DEFAULT_ECM_BITRATE 30000
#define ASYNC_HANDLER_EXTRA_STACK_SIZE (1024 * 1024)


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

// Notes on crypto-period dynamics:
//
// A crypto-period is defined using a CryptoPeriod object (private class inside
// ScramblerPlugin). It contains: crypto-period number, current/next CW and ECM
// containing these two CW.
//
// It is necessary to maintain two CryptoPeriod objects.
// During crypto-period N, designated as cp(N):
// - Scrambling is performed using CW(N).
// - At beginning of cp(N), if delay_start > 0, we broadcast ECM(N-1).
// - In middle of cp(N), we broadcast ECM(N).
// - At end of cp(N), if delay_start < 0, we broadcast ECM(N+1).
//
// So, during cp(N), we need cp(N-1)/cp(N), then cp(N)/cp(N+1). On a dynamic
// standpoint, as soon as ECM(N-1) is no longer needed, we generate cp(N+1).
// In asynchronous mode, there is enough time to generate ECM(N+1) while
// cp(N) is finishing.
//
// The transition points in the TS are:
// - CW change (start a new crypto-period)
// - ECM change (start broadcasting a new ECM, can be before or after
//   start of crypto-period, depending on delay_start).
//
// Entering "degraded mode":
// In asynchronous mode (the default), an ECM is actually returned by the ECMG
// long after it has been submitted. To complete a transition CW(N) -> CW(N+1)
// or ECM(N) -> ECM(N+1), we check that ECM(N+1) is ready. If it is not, we
// enter "degraded mode". In this mode, no transition is allowed, the same CW
// and ECM are used until exit of the degraded mode. This can occur when an
// ECM takes too long to be ciphered.
//
// Exiting "degraded mode":
// When in degraded mode, each time an ECM(N) packet is inserted, we check if
// ECM(N+1) is ready. When it is ready, we exit degraded mode. If delay_start
// is negative, we immediately perform an ECM transition and we recompute the
// time for the next CW transition. If delay_start is positive, we immediately
// perform a CW transition and we recompute the time for the next ECM transition.

namespace ts {
    class ScramblerPlugin:
        public ProcessorPlugin,
        private PMTHandlerInterface
    {
    public:
        // Implementation of plugin API
        ScramblerPlugin(TSP*);
        virtual bool start() override;
        virtual bool stop() override;
        virtual Status processPacket(TSPacket&, bool&, bool&) override;

    private:
        // Description of a crypto-period.
        // Each CryptoPeriod object points to its ScramblerPlugin parent object.
        // In case of error in a CryptoPeriod object, the _abort volatile flag
        // is set in ScramblerPlugin.
        class CryptoPeriod: private ECMGClientHandlerInterface
        {
        public:
            // Default constructor.
            CryptoPeriod();

            // Initialize first crypto period.
            // Generate two randow CW and corresponding ECM.
            // ECM generation may complete asynchronously.
            void initCycle(ScramblerPlugin*, uint16_t cp_number);

            // Initialize crypto period following specified one.
            // ECM generation may complete asynchronously.
            void initNext(const CryptoPeriod&);

            // Check if ECM generation is complete (useful in asynchronous mode)
            bool ecmReady() const { return _ecm_ok; }

            // Get next ECM packet in ECM cycle (or null packet if ECM not ready).
            void getNextECMPacket(TSPacket&);

            // Initialize the scrambler with the current control word.
            bool initScramblerKey() const;

        private:
            ScramblerPlugin* _scrambler;      // Reference to scrambler plugin
            uint16_t         _cp_number;      // Crypto-period number
            volatile bool    _ecm_ok;         // _ecm field is valid
            TSPacketVector   _ecm;            // Packetized ECM
            size_t           _ecm_pkt_index;  // Next ECM packet to insert in TS
            ByteBlock        _cw_current;
            ByteBlock        _cw_next;

            // Generate the ECM for a crypto-period.
            // With --synchronous, the ECM is directly generated. Otherwise,
            // the ECM will be set later, notified through private handleECM.
            void generateECM();

            // Invoked when an ECM is available, maybe in the context of an external thread.
            virtual void handleECM(const ecmgscs::ECMResponse&) override;

            // Inaccessible operations
            CryptoPeriod(const CryptoPeriod&) = delete;
            CryptoPeriod& operator=(const CryptoPeriod&) = delete;
        };

        // ScramblerPlugin parameters, remain constant after start()
        ServiceDiscovery  _service;             // Service description
        bool              _use_service;         // Scramble a service (ie. not a specific list of PID's).
        bool              _component_level;     // Insert CA_descriptors at component level
        bool              _scramble_audio;      // Scramble all audio components
        bool              _scramble_video;      // Scramble all video components
        bool              _scramble_subtitles;  // Scramble all subtitles components
        bool              _synchronous_ecmg;    // Synchronous ECM generation
        bool              _ignore_scrambled;    // Ignore packets which are already scrambled
        bool              _update_pmt;          // Update PMT.
        bool              _need_cp;             // Need to manage crypto-periods (ie. not one single fixed CW).
        bool              _need_ecm;            // Need to manage ECM insertion (ie. not fixed CW's).
        SocketAddress     _ecmg_addr;           // ECMG socket address
        uint32_t          _super_cas_id;        // CA system & subsystem id
        ByteBlock         _access_criteria;     // AC constant value
        ByteBlock         _ca_desc_private;     // Private data to insert in CA_descriptor
        MilliSecond       _cp_duration;         // Crypto-period duration
        MilliSecond       _delay_start;         // Delay between CP start and ECM start (can be negative)
        BitRate           _ecm_bitrate;         // ECM PID's bitrate
        PID               _ecm_pid;             // PID for ECM
        PacketCounter     _partial_scrambling;  // Do not scramble all packets if > 1
        tlv::Logger       _logger;              // Message logger for ECMG <=> SCS protocol
        ecmgscs::ChannelStatus _channel_status; // Initial response to ECMG channel_setup
        ecmgscs::StreamStatus  _stream_status;  // Initial response to ECMG stream_setup

        // ScramblerPlugin state
        volatile bool     _abort;               // Error (service not found, etc)
        bool              _degraded_mode;       // In degraded mode (see comments above)
        PacketCounter     _packet_count;        // Complete TS packet counter
        PacketCounter     _scrambled_count;     // Summary of scrambled packets
        PacketCounter     _partial_clear;       // How many clear packets to keep clear
        PacketCounter     _pkt_insert_ecm;      // Insertion point for next ECM packet.
        PacketCounter     _pkt_change_cw;       // Transition point for next CW change
        PacketCounter     _pkt_change_ecm;      // Transition point for next ECM change
        BitRate           _ts_bitrate;          // Saved TS bitrate
        ECMGClient        _ecmg;                // Connection with the ECMG
        uint8_t           _ecm_cc;              // Continuity counter in ECM PID.
        PIDSet            _scrambled_pids;      // List of pids to scramble
        PIDSet            _conflict_pids;       // List of pids to scramble with scrambled input packets
        PIDSet            _input_pids;          // List of input pids
        CryptoPeriod      _cp[2];               // Previous/current or current/next crypto-periods
        size_t            _current_cw;          // Index to current CW (current crypto period)
        size_t            _current_ecm;         // Index to current ECM (ECM being broadcast)
        TSScrambling      _scrambling;          // Scrambler
        CyclingPacketizer _pzer_pmt;            // Packetizer for modified PMT

        // Return current/next CryptoPeriod for CW or ECM
        CryptoPeriod& currentCW()  { return _cp[_current_cw]; }
        CryptoPeriod& nextCW()     { return _cp[(_current_cw + 1) & 0x01]; }
        CryptoPeriod& currentECM() { return _cp[_current_ecm]; }
        CryptoPeriod& nextECM()    { return _cp[(_current_ecm + 1) & 0x01]; }

        // Perform CW and ECM transition
        bool changeCW();
        void changeECM();

        // Check if we are in degraded mode or if we enter degraded mode
        bool inDegradedMode();

        // Try to exit from degraded mode
        bool tryExitDegradedMode();

        // Invoked when the PMT of the service is available.
        virtual void handlePMT(const PMT&) override;

        // Inaccessible operations
        ScramblerPlugin() = delete;
        ScramblerPlugin(const ScramblerPlugin&) = delete;
        ScramblerPlugin& operator=(const ScramblerPlugin&) = delete;
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_PROCESSOR(scrambler, ts::ScramblerPlugin)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::ScramblerPlugin::ScramblerPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"DVB scrambler", u"[options] [service]"),
    _service(this, *tsp),
    _use_service(false),
    _component_level(false),
    _scramble_audio(false),
    _scramble_video(false),
    _scramble_subtitles(false),
    _synchronous_ecmg(false),
    _ignore_scrambled(false),
    _update_pmt(false),
    _need_cp(false),
    _need_ecm(false),
    _ecmg_addr(),
    _super_cas_id(0),
    _access_criteria(),
    _ca_desc_private(),
    _cp_duration(0),
    _delay_start(0),
    _ecm_bitrate(0),
    _ecm_pid(PID_NULL),
    _partial_scrambling(0),
    _logger(ts::Severity::Debug, tsp_),
    _channel_status(),
    _stream_status(),
    _abort(false),
    _degraded_mode(false),
    _packet_count(0),
    _scrambled_count(0),
    _partial_clear(0),
    _pkt_insert_ecm(0),
    _pkt_change_cw(0),
    _pkt_change_ecm(0),
    _ts_bitrate(0),
    _ecmg(ASYNC_HANDLER_EXTRA_STACK_SIZE),
    _ecm_cc(0),
    _scrambled_pids(),
    _conflict_pids(),
    _input_pids(),
    _cp(),
    _current_cw(0),
    _current_ecm(0),
    _scrambling(*tsp),
    _pzer_pmt()
{
    option(u"",                      0,  STRING, 0, 1);
    option(u"access-criteria",      'a', STRING);
    option(u"bitrate-ecm",          'b', POSITIVE);
    option(u"channel-id",            0,  UINT16);
    option(u"component-level",       0);
    option(u"cp-duration",          'd', POSITIVE);
    option(u"ecm-id",               'i', UINT16);
    option(u"ecmg",                 'e', STRING);
    option(u"ecmg-scs-version",     'v', INTEGER, 0, 1, 2, 3);
    option(u"ignore-scrambled",      0);
    option(u"log-data",              0,  ts::Severity::Enums, 0, 1, true);
    option(u"log-protocol",          0,  ts::Severity::Enums, 0, 1, true);
    option(u"no-audio",              0);
    option(u"no-video",              0);
    option(u"partial-scrambling",    0,  POSITIVE);
    option(u"pid",                  'p', PIDVAL, 0, UNLIMITED_COUNT);
    option(u"pid-ecm",               0,  PIDVAL);
    option(u"private-data",          0,  STRING);
    option(u"stream-id",             0,  UINT16);
    option(u"subtitles",             0);
    option(u"super-cas-id",         's', UINT32);
    option(u"synchronous",           0);

    setHelp(u"Service parameter:\n"
            u"\n"
            u"  Specifies the optional service to scramble. If no service is specified, a\n"
            u"  list of PID's to scramble must be provided using --pid options. When PID's\n"
            u"  are provided, fixed control words must be specified as well.\n"
            u"\n"
            u"  If no fixed CW is specified, a random CW is generated for each crypto-period\n"
            u"  and ECM's containing the current and next CW's are created and inserted in\n"
            u"  the stream. ECM's can be created only when a service is specified.\n"
            u"\n"
            u"  If the argument is an integer value (either decimal or hexadecimal), it is\n"
            u"  interpreted as a service id. Otherwise, it is interpreted as a service name,\n"
            u"  as specified in the SDT. The name is not case sensitive and blanks are\n"
            u"  ignored. If the input TS does not contain an SDT, use service ids only.\n"
            u"\n"
            u"General options:\n"
            u"\n"
            u"  -a value\n"
            u"  --access-criteria value\n"
            u"      Specifies the access criteria for the service as sent to the ECMG.\n"
            u"      The value must be a suite of hexadecimal digits.\n"
            u"\n"
            u"  -b value\n"
            u"  --bitrate-ecm value\n"
            u"      Specifies the bitrate for ECM PID's in bits / second. The default is\n"
            u"      " + UString::Decimal(DEFAULT_ECM_BITRATE) + u" b/s.\n"
            u"\n"
            u"  --channel-id value\n"
            u"      Specifies the DVB SimulCrypt ECM_channel_id for the ECMG (default: 1).\n"
            u"\n"
            u"  -d seconds\n"
            u"  --cp-duration seconds\n"
            u"      Specifies the crypto-period duration in seconds (default: 10).\n"
            u"\n"
            u"  --component-level\n"
            u"      Add CA_descriptors at component level in the PMT. By default, the\n"
            u"      CA_descriptor is added at program level.\n"
            u"\n"
            u"  -i value\n"
            u"  --ecm-id value\n"
            u"      Specifies the DVB SimulCrypt ECM_id for the ECMG (default: 1).\n"
            u"\n"
            u"  -e host:port\n"
            u"  --ecmg host:port\n"
            u"      Specify an ECM Generator. Without ECMG, a fixed control word must be\n"
            u"      specified using --control-word.\n"
            u"\n"
            u"  -v value\n"
            u"  --ecmg-scs-version value\n"
            u"      Specifies the version of the ECMG <=> SCS DVB SimulCrypt protocol.\n"
            u"      Valid values are 2 and 3. The default is 2.\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
            u"\n"
            u"  --ignore-scrambled\n"
            u"      Ignore packets which are already scrambled. Since these packets\n"
            u"      are likely scrambled with a different control word, descrambling\n"
            u"      will not be possible the usual way.\n"
            u"\n"
            u"  --log-data[=level]\n"
            u"      Same as --log-protocol but applies to CW_provision and ECM_response\n"
            u"      messages only. To debug the session management without being flooded by\n"
            u"      data messages, use --log-protocol=info --log-data=debug.\n"
            u"\n"
            u"  --log-protocol[=level]\n"
            u"      Log all ECMG <=> SCS protocol messages using the specified level. If the\n"
            u"      option is not present, the messages are logged at debug level only. If the\n"
            u"      option is present without value, the messages are logged at info level.\n"
            u"      A level can be a numerical debug level or any of the following:\n"
            u"      " + ts::Severity::Enums.nameList() + u".\n"
            u"\n"
            u"  --no-audio\n"
            u"      Do not scramble audio components in the selected service. By default,\n"
            u"      all audio components are scrambled.\n"
            u"\n"
            u"  --no-video\n"
            u"      Do not scramble video components in the selected service. By default,\n"
            u"      all video components are scrambled.\n"
            u"\n"
            u"  --partial-scrambling count\n"
            u"      Do not scramble all packets, only one packet every \"count\" packets.\n"
            u"      The default value is 1, meaning that all packets are scrambled.\n"
            u"      Specifying higher values is a way to reduce the scrambling CPU load\n"
            u"      while keeping the service mostly scrambled.\n"
            u"\n"
            u"  -p value\n"
            u"  --pid value\n"
            u"      Scramble packets with this PID value. Several -p or --pid options may be\n"
            u"      specified. By default, scramble the specified service.\n"
            u"\n"
            u"  --pid-ecm value\n"
            u"      Specifies the new ECM PID for the service. By defaut, use the first\n"
            u"      unused PID immediately following the PMT PID. Using the default, there\n"
            u"      is a risk to later discover that this PID is already used. In that case,\n"
            u"      specify --pid-ecm with a notoriously unused PID value.\n"
            u"\n"
            u"  --private-data value\n"
            u"      Specifies the private data to insert in the CA_descriptor in the PMT.\n"
            u"      The value must be a suite of hexadecimal digits.\n"
            u"\n"
            u"  --stream-id value\n"
            u"      Specifies the DVB SimulCrypt ECM_stream_id for the ECMG (default: 1).\n"
            u"\n"
            u"  --subtitles\n"
            u"      Scramble subtitles components in the selected service. By default, the\n"
            u"      subtitles components are not scrambled.\n"
            u"\n"
            u"  -s value\n"
            u"  --super-cas-id value\n"
            u"      Specify the DVB SimulCrypt Super_CAS_Id. This is required when --ecmg\n"
            u"      is specified.\n"
            u"\n"
            u"  --synchronous\n"
            u"      Specify to synchronously generate the ECM's. By default, in real-time\n"
            u"      mode, the packet processing continues while generating ECM's. This option\n"
            u"      is always on in offline mode.\n"
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n");

    _scrambling.defineOptions(*this);
    _scrambling.addHelp(*this);
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::start()
{
    // Reset states
    _conflict_pids.reset();
    _packet_count = 0;
    _scrambled_count = 0;
    _ecm_cc = 0;
    _abort = false;
    _degraded_mode = false;
    _ts_bitrate = 0;
    _pkt_insert_ecm = 0;
    _pkt_change_cw = 0;
    _pkt_change_ecm = 0;
    _partial_clear = 0;
    _update_pmt = false;

    // Parameters
    _use_service = present(u"");
    _service.set(value(u""));
    getPIDSet(_scrambled_pids, u"pid");
    _synchronous_ecmg = present(u"synchronous") || !tsp->realtime();
    _component_level = present(u"component-level");
    _scramble_audio = !present(u"no-audio");
    _scramble_video = !present(u"no-video");
    _scramble_subtitles = present(u"subtitles");
    _partial_scrambling = intValue<PacketCounter>(u"partial-scrambling", 1);
    _ignore_scrambled = present(u"ignore-scrambled");
    _ecm_pid = intValue<PID>(u"pid-ecm", PID_NULL);
    _ecm_bitrate = intValue<BitRate>(u"bitrate-ecm", DEFAULT_ECM_BITRATE);
    _cp_duration = 1000 * intValue<MilliSecond>(u"cp-duration", 10);
    _delay_start = 0;
    _super_cas_id = intValue<uint32_t>(u"super-cas-id");
    const uint16_t ecm_channel_id = intValue<uint16_t>(u"channel-id", 1);
    const uint16_t ecm_stream_id = intValue<uint16_t>(u"stream-id", 1);
    const uint16_t ecm_id = intValue<uint16_t>(u"ecm-id", 1);

    // Set logging levels.
    const int log_protocol = present(u"log-protocol") ? intValue<int>(u"log-protocol", ts::Severity::Info) : ts::Severity::Debug;
    const int log_data = present(u"log-data") ? intValue<int>(u"log-data", ts::Severity::Info) : log_protocol;
    _logger.setDefaultSeverity(log_protocol);
    _logger.setSeverity(ts::ecmgscs::Tags::CW_provision, log_data);
    _logger.setSeverity(ts::ecmgscs::Tags::ECM_response, log_data);

    // Scrambling-specific parameters.
    if (!_scrambling.loadArgs(*this)) {
        return false;
    }

    // Decode hexa data.
    if (!value(u"access-criteria").hexaDecode(_access_criteria)) {
        tsp->error(u"invalid access criteria, specify an even number of hexa digits");
        return false;
    }
    if (!value(u"private-data").hexaDecode(_ca_desc_private)) {
        tsp->error(u"invalid private data for CA_descriptor, specify an even number of hexa digits");
        return false;
    }

    // Scramble either a service or a list of PID's, not a mixture of them.
    if ((_use_service + _scrambled_pids.any()) != 1) {
        tsp->error(u"specify either a service or a list of PID's");
        return false;
    }

    // To scramble a fixed list of PID's, we need fixed control words, otherwise the random CW's are lost.
    if (_scrambled_pids.any() && !_scrambling.hasFixedCW()) {
        tsp->error(u"specify control words to scramble an explicit list of PID's");
        return false;
    }

    // Do we need to manage crypto-periods and ECM insertion?
    _need_cp = _scrambling.fixedCWCount() != 1;
    _need_ecm = _use_service && !_scrambling.hasFixedCW();

    // Specify which ECMG <=> SCS version to use.
    ecmgscs::Protocol::Instance()->setVersion(intValue<tlv::VERSION>(u"ecmg-scs-version", 2));

    // Initialize ECMG.
    if (_need_ecm) {
        if (!present(u"ecmg")) {
            // Without fixed control word and ECMG, we cannot do anything.
            tsp->error(u"specify either --cw, --cw-file or --ecmg");
            return false;
        }
        else if (!_ecmg_addr.resolve(value(u"ecmg"), *tsp)) {
            // Invalid host:port, error message already reported
            return false;
        }
        else if (!present(u"super-cas-id")) {
            tsp->error(u"--super-cas-id is required with --ecmg");
            return false;
        }
        else if (!_ecmg.connect(_ecmg_addr, _super_cas_id, ecm_channel_id, ecm_stream_id, ecm_id,
                                uint16_t(_cp_duration / 100), _channel_status, _stream_status, tsp, _logger))
        {
            // Error connecting to ECMG, error message already reported
            return false;
        }
        else {
            // Now correctly connected to ECMG.
            // Validate delay start (limit to half the crypto-period).
            _delay_start = MilliSecond(_channel_status.delay_start);
            if (_delay_start > _cp_duration / 2 || _delay_start < -_cp_duration / 2) {
                tsp->error(u"crypto-period too short for this CAS, must be at least %'d ms.", {2 * std::abs(_delay_start)});
                return false;
            }
            tsp->debug(u"crypto-period duration: %'d ms, delay start: %'d ms", {_cp_duration, _delay_start});

            // Create first and second crypto-periods
            _current_cw = 0;
            _current_ecm = 0;
            _cp[0].initCycle(this, 0);
            if (!_cp[0].initScramblerKey()) {
                return false;
            }
            _cp[1].initNext(_cp[0]);
        }
    }

    // The PMT will be modified, initialize the PMT packetizer.
    // Note that even without ECMG we may need to add a scrambling_descriptor in the PMT.
    _pzer_pmt.reset();
    _pzer_pmt.setStuffingPolicy(CyclingPacketizer::ALWAYS);

    // Initialize the list of used pids. Preset reserved PIDs.
    _input_pids.reset();
    _input_pids.set(PID_NULL);
    for (PID pid = 0; pid <= 0x001F; ++pid) {
        _input_pids.set(pid);
    }

    return !_abort;
}


//----------------------------------------------------------------------------
// Stop method
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::stop()
{
    // Disconnect from ECMG
    if (_ecmg.isConnected()) {
        _ecmg.disconnect();
    }

    tsp->debug(u"scrambled %'d packets in %'d PID's", {_scrambled_count, _scrambled_pids.count()});
    return true;
}


//----------------------------------------------------------------------------
//  This method processes the PMT of the service.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::handlePMT(const PMT& table)
{
    assert(_use_service);

    // We need to know the bitrate in order to schedule crypto-periods or ECM insertion.
    if (_ts_bitrate == 0 && (_need_cp || _need_ecm)) {
        tsp->error(u"unknown bitrate, cannot schedule crypto-periods");
        _abort = true;
        return;
    }

    // Need a modifiable version of the PMT.
    PMT pmt(table);

    // Collect all PIDS to scramble.
    _scrambled_pids.reset();
    for (PMT::StreamMap::const_iterator it = pmt.streams.begin(); it != pmt.streams.end(); ++it) {
        const PID pid = it->first;
        const PMT::Stream& stream(it->second);
        _input_pids.set(pid);
        if ((_scramble_audio && stream.isAudio()) || (_scramble_video && stream.isVideo()) || (_scramble_subtitles && stream.isSubtitles())) {
            _scrambled_pids.set(pid);
            tsp->verbose(u"starting scrambling PID 0x%X", {pid});
        }
    }

    // Check that we have somethng to scramble.
    if (_scrambled_pids.none()) {
        tsp->error(u"no PID to scramble in service");
        _abort = true;
        return;
    }

    // Allocate a PID value for ECM if necessary
    if (_need_ecm && _ecm_pid == PID_NULL) {
        // Start at service PMT PID, then look for an unused one.
        for (_ecm_pid = _service.getPMTPID() + 1; _ecm_pid < PID_NULL && _input_pids.test(_ecm_pid); _ecm_pid++) {}
        if (_ecm_pid >= PID_NULL) {
            tsp->error(u"cannot find an unused PID for ECM, try --pid-ecm");
            _abort = true;
        }
        else {
            tsp->verbose(u"using PID %d (0x%X) for ECM", {_ecm_pid, _ecm_pid});
        }
    }

    // Add a scrambling_descriptor in the PMT for scrambling other than DVB-CSA2.
    if (_scrambling.scramblingType() != SCRAMBLING_DVB_CSA2) {
        _update_pmt = true;
        pmt.descs.add(ScramblingDescriptor(_scrambling.scramblingType()));
    }

    // With ECM generation, modify the PMT
    if (_need_ecm) {
        _update_pmt = true;

        // Create a CA_descriptor
        CADescriptor ca_desc((_super_cas_id >> 16) & 0xFFFF, _ecm_pid);
        ca_desc.private_data = _ca_desc_private;

        // Add the CA_descriptor at program level or component level
        if (_component_level) {
            // Add a CA_descriptor in each scrambled component
            for (PMT::StreamMap::iterator it = pmt.streams.begin(); it != pmt.streams.end(); ++it) {
                if (_scrambled_pids.test(it->first)) {
                    it->second.descs.add(ca_desc);
                }
            }
        }
        else {
            // Add one single CA_descriptor at program level
            pmt.descs.add(ca_desc);
        }
    }

    // Packetize the modified PMT
    if (_update_pmt) {
        _pzer_pmt.removeSections(TID_PMT, pmt.service_id);
        _pzer_pmt.setPID(_service.getPMTPID());
        _pzer_pmt.addTable(pmt);
    }

    // Next crypto-period.
    if (_need_cp) {
        _pkt_change_cw = _packet_count + PacketDistance(_ts_bitrate, _cp_duration);
    }

    // Initialize ECM insertion.
    if (_need_ecm) {

        // Insert current ECM packets as soon as possible.
        _pkt_insert_ecm = _packet_count;

        // Next ECM may start before or after next crypto-period
        _pkt_change_ecm = _delay_start > 0 ?
            _pkt_change_cw + PacketDistance(_ts_bitrate, _delay_start) :
            _pkt_change_cw - PacketDistance(_ts_bitrate, _delay_start);
    }
}


//----------------------------------------------------------------------------
// Check if we are in degraded mode or if we enter degraded mode
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::inDegradedMode()
{
    if (!_need_ecm) {
        // No ECM, no degraded mode.
        return false;
    }
    else if (_degraded_mode) {
        // Already in degraded mode, do not try to exit from it now.
        return true;
    }
    else if (nextECM().ecmReady()) {
        // Next ECM ready, no need to enter degraded mode.
        return false;
    }
    else {
        // Entering degraded mode
        tsp->warning(u"Next ECM not ready, entering degraded mode");
        return _degraded_mode = true;
    }
}


//----------------------------------------------------------------------------
// Try to exit from degraded mode
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::tryExitDegradedMode()
{
    // If not in degraded mode, nothing to do
    if (!_degraded_mode) {
        return true;
    }
    assert(_need_ecm);

    // We are in degraded mode. If next ECM not yet ready, stay degraded
    if (!nextECM().ecmReady()) {
        return true;
    }

    // Next ECM is ready, at last. Exit degraded mode.
    tsp->info(u"Next ECM ready, exiting from degraded mode");
    _degraded_mode = false;

    // Compute next CW and ECM change.
    if (_delay_start < 0) {
        // Start broadcasting ECM before beginning of crypto-period, ie. now
        changeECM();
        // Postpone CW change
        _pkt_change_cw = _packet_count + PacketDistance(_ts_bitrate, _delay_start);
    }
    else {
        // Change CW now.
        if (!changeCW()) {
            return false;
        }
        // Start broadcasting ECM after beginning of crypto-period
        _pkt_change_ecm = _packet_count + PacketDistance(_ts_bitrate, _delay_start);
    }

    return true;
}


//----------------------------------------------------------------------------
// Perform crypto-period transition, for CW or ECM
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::changeCW()
{
    // Allowed to change CW only if not in degraded mode
    if (!inDegradedMode()) {

        // Point to next crypto-period
        _current_cw = (_current_cw + 1) & 0x01;

        // Use new control word
        if (!currentCW().initScramblerKey()) {
            return false;
        }

        // Determine new transition point.
        if (_need_cp) {
            _pkt_change_cw = _packet_count + PacketDistance(_ts_bitrate, _cp_duration);
        }

        // Generate (or start generating) next ECM when using ECM(N) in cp(N)
        if (_need_ecm && _current_ecm == _current_cw) {
            nextCW().initNext(currentCW());
        }
    }
    return true;
}

void ts::ScramblerPlugin::changeECM()
{
    // Allowed to change CW only if not in degraded mode
    if (_need_ecm && !inDegradedMode()) {

        // Point to next crypto-period
        _current_ecm = (_current_ecm + 1) & 0x01;

        // Determine new transition point
        _pkt_change_ecm = _packet_count + PacketDistance(_ts_bitrate, _cp_duration);

        // Generate (or start generating) next ECM when using ECM(N) in cp(N)
        if (_current_ecm == _current_cw) {
            nextCW().initNext(currentCW());
        }
    }
}


//----------------------------------------------------------------------------
// Packet processing method
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::ScramblerPlugin::processPacket(TSPacket& pkt, bool& flush, bool& bitrate_changed)
{
    // Count packets
    _packet_count++;

    // Track all input PIDs
    const PID pid = pkt.getPID();
    _input_pids.set(pid);

    // Maintain bitrate, keep previous one if unknown
    const BitRate br = tsp->bitrate();
    if (br != 0) {
        _ts_bitrate = br;
    }

    // Filter interesting sections to discover the service.
    if (_use_service) {
        _service.feedPacket(pkt);
    }

    // If the service is definitely unknown or a fatal error occured during PMT analysis, give up.
    if (_abort || _service.nonExistentService()) {
        return TSP_END;
    }

    // Abort if allocated PID for ECM is already present in TS
    if (_ecm_pid != PID_NULL && pid == _ecm_pid) {
        tsp->error(u"ECM PID allocation conflict, used 0x%X, now found as input PID, try another --pid-ecm", {pid});
        return TSP_END;
    }

    // As long as we do not know which PID's to scramble, nullify all packets
    if (_scrambled_pids.none()) {
        return TSP_NULL;
    }

    // Packetize modified PMT when needed.
    if (_update_pmt && pid == _pzer_pmt.getPID()) {
        _pzer_pmt.getNextPacket(pkt);
        return TSP_OK;
    }

    // Is it time to apply the next control word ?
    if (_need_cp && _packet_count >= _pkt_change_cw && !changeCW()) {
        return TSP_END;
    }

    // Is it time to start broadcasting the next ECM ?
    if (_need_ecm && _packet_count >= _pkt_change_ecm) {
        changeECM();
    }

    // Insert an ECM packet (replace a null packet) when time to do so
    if (_need_ecm && pid == PID_NULL && _packet_count >= _pkt_insert_ecm) {

        // Compute next insertion point (approximate)
        assert(_ecm_bitrate != 0);
        _pkt_insert_ecm += BitRate(_ts_bitrate / _ecm_bitrate);

        // Try to exit from degraded mode, if we were in.
        // Note that return false means unrecoverable error here.
        if (!tryExitDegradedMode()) {
            return TSP_END;
        }

        // Replace current null packet with an ECM packet
        currentECM().getNextECMPacket(pkt);
        return TSP_OK;
    }

    // If the packet has no payload or its PID is not to be scrambled, there is nothing to do.
    if (!pkt.hasPayload() || !_scrambled_pids.test(pid)) {
        return TSP_OK;
    }

    // If packet is already scrambled, error or ignore (do not modify packet)
    if (pkt.isScrambled()) {
        if (_ignore_scrambled) {
            if (!_conflict_pids.test(pid)) {
                tsp->verbose(u"found input scrambled packets in PID %d (0x%X), ignored", {pid, pid});
                _conflict_pids.set(pid);
            }
            return TSP_OK;
        }
        else {
            tsp->error(u"packet already scrambled in PID %d (0x%X)", {pid, pid});
            return TSP_END;
        }
    }

    // Manage partial scrambling
    if (_partial_clear > 0) {
        // Do not scramble this packet
        _partial_clear--;
        return TSP_OK;
    }
    else {
        // Scramble this packet and reinit subsequent number of packets to keep clear
        _partial_clear = _partial_scrambling - 1;
    }

    // Scramble the packet payload.
    if (!_scrambling.encrypt(pkt)) {
        return TSP_END;
    }
    _scrambled_count++;

    return TSP_OK;
}


//----------------------------------------------------------------------------
// CryptoPeriod default constructor.
//----------------------------------------------------------------------------

ts::ScramblerPlugin::CryptoPeriod::CryptoPeriod() :
    _scrambler(0),
    _cp_number(0),
    _ecm_ok(false),
    _ecm(),
    _ecm_pkt_index(0),
    _cw_current(),
    _cw_next()
{
}


//----------------------------------------------------------------------------
// Initialize first crypto period.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::initCycle(ScramblerPlugin* scrambler, uint16_t cp_number)
{
    _scrambler = scrambler;
    _cp_number = cp_number;

    if (_scrambler->_need_ecm) {
        BetterSystemRandomGenerator::Instance()->readByteBlock(_cw_current, _scrambler->_scrambling.cwSize());
        BetterSystemRandomGenerator::Instance()->readByteBlock(_cw_next,_scrambler->_scrambling.cwSize());
        generateECM();
    }
}


//----------------------------------------------------------------------------
// Initialize crypto period following specified one.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::initNext(const CryptoPeriod& previous)
{
    _scrambler = previous._scrambler;
    _cp_number = previous._cp_number + 1;

    if (_scrambler->_need_ecm) {
        _cw_current = previous._cw_next;
        BetterSystemRandomGenerator::Instance()->readByteBlock(_cw_next, _scrambler->_scrambling.cwSize());
        generateECM();
    }
}


//----------------------------------------------------------------------------
// Initialize the scrambler with the current control word.
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::CryptoPeriod::initScramblerKey() const
{
    // Change the parity of the scrambled packets.
    // Set our random current control word if no fixed CW.
    return _scrambler->_scrambling.setEncryptParity(_cp_number) &&
        (!_scrambler->_need_ecm || _scrambler->_scrambling.setCW(_cw_current, _cp_number));
}


//----------------------------------------------------------------------------
// Generate the ECM for a crypto-period.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::generateECM()
{
    _ecm_ok = false;

    if (_scrambler->_synchronous_ecmg) {
        // Synchronous ECM generation
        ecmgscs::ECMResponse response;
        if (!_scrambler->_ecmg.generateECM(_cp_number,
                                           _cw_current,
                                           _cw_next,
                                           _scrambler->_access_criteria,
                                           uint16_t(_scrambler->_cp_duration / 100),
                                           response)) {
            // Error, message already reported
            _scrambler->_abort = true;
        }
        else {
            handleECM(response);
        }
    }
    else {
        // Asynchronous ECM generation
        if (!_scrambler->_ecmg.submitECM(_cp_number,
                                         _cw_current,
                                         _cw_next,
                                         _scrambler->_access_criteria,
                                         uint16_t(_scrambler->_cp_duration / 100),
                                         this)) {
            // Error, message already reported
            _scrambler->_abort = true;
        }
    }
}


//----------------------------------------------------------------------------
// Invoked when an ECM is available, maybe in the context of an external thread
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::handleECM(const ecmgscs::ECMResponse& response)
{
    if (_scrambler->_channel_status.section_TSpkt_flag == 0) {
        // ECMG returns ECM in section format
        SectionPtr sp(new Section(response.ECM_datagram));
        if (!sp->isValid()) {
            _scrambler->tsp->error(u"ECMG returned an invalid ECM section (%d bytes)", {response.ECM_datagram.size()});
            _scrambler->_abort = true;
            return;
        }
        // Packetize the section
        OneShotPacketizer pzer(_scrambler->_ecm_pid, true);
        pzer.addSection(sp);
        pzer.getPackets(_ecm);

    }
    else if (response.ECM_datagram.size() % PKT_SIZE != 0) {
        // ECMG returns ECM in packet format, but not an integral number of packets
        _scrambler->tsp->error(u"invalid ECM size (%d bytes), not a multiple of %d", {response.ECM_datagram.size(), PKT_SIZE});
        _scrambler->_abort = true;
        return;
    }
    else {
        // ECMG returns ECM in packet format
        _ecm.resize(response.ECM_datagram.size() / PKT_SIZE);
        ::memcpy(&_ecm[0].b, response.ECM_datagram.data(), response.ECM_datagram.size());  // Flawfinder: ignore: memcpy()
    }

    _scrambler->tsp->debug(u"got ECM for crypto-period %d, %d packets", {_cp_number, _ecm.size()});

    _ecm_pkt_index = 0;

    // Last instruction: set the volatile boolean
    _ecm_ok = true;
}


//----------------------------------------------------------------------------
// Get next ECM packet
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::getNextECMPacket (TSPacket& pkt)
{
    if (!_ecm_ok || _ecm.size() == 0) {
        // No ECM, return a null packet
        pkt = NullPacket;
    }
    else {
        // Copy ECM packet
        assert(_ecm_pkt_index < _ecm.size());
        pkt = _ecm[_ecm_pkt_index];
        // Move to next ECM packet
        if (++_ecm_pkt_index >= _ecm.size()) {
            _ecm_pkt_index = 0;
        }
        // Adjust PID and continuity counter in TS packet
        pkt.setPID(_scrambler->_ecm_pid);
        pkt.setCC(_scrambler->_ecm_cc);
        _scrambler->_ecm_cc = (_scrambler->_ecm_cc + 1) & 0x0F;
    }
}
