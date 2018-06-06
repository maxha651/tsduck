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
//  Null packets input
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsPluginRepository.h"
TSDUCK_SOURCE;


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class NullInput: public InputPlugin
    {
    public:
        // Implementation of plugin API
        NullInput(TSP*);
        virtual bool start() override;
        virtual size_t receive(TSPacket*, size_t) override;

    private:
        PacketCounter _max_count;   // Number of packets to generate
        PacketCounter _count;       // Number of generated packets

        // Inaccessible operations
        NullInput() = delete;
        NullInput(const NullInput&) = delete;
        NullInput& operator=(const NullInput&) = delete;
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_INPUT(null, ts::NullInput)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::NullInput::NullInput(TSP* tsp_) :
    InputPlugin(tsp_, u"Generate null packets", u"[options] [count]"),
    _max_count(0),
    _count(0)
{
    option(u"",                   0,  UNSIGNED, 0, 1);
    option(u"joint-termination", 'j');

    setHelp(u"Count:\n"
            u"  Specify the number of null packets to generate. After the last packet,\n"
            u"  an end-of-file condition is generated. By default, if count is not\n"
            u"  specified, null packets are generated endlessly.\n"
            u"\n"
            u"Options:\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
            u"\n"
            u"  -j\n"
            u"  --joint-termination\n"
            u"      When the number of null packets is specified, perform a \"joint\n"
            u"      termination\" when completed instead of unconditional termination.\n"
            u"      See \"tsp --help\" for more details on \"joint termination\".\n"
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n");
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::NullInput::start()
{
    tsp->useJointTermination(present(u"joint-termination"));
    _max_count = intValue<PacketCounter>(u"", std::numeric_limits<PacketCounter>::max());
    _count = 0;
    return true;
}


//----------------------------------------------------------------------------
// Input method
//----------------------------------------------------------------------------

size_t ts::NullInput::receive (TSPacket* buffer, size_t max_packets)
{
    // If "joint termination" reached for this plugin
    if (_count >= _max_count && tsp->useJointTermination()) {
        // Declare terminated
        tsp->jointTerminate();
        // Continue generating null packets until completion of tsp (suppress max packet count)
        _max_count = std::numeric_limits<PacketCounter>::max();
    }

    // Fill buffer
    size_t n;
    for (n = 0; n < max_packets && _count++ < _max_count; buffer[n++] = NullPacket) {}
    return n;
}
