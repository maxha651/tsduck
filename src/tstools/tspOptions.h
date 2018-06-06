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
//!
//!  @file
//!  Transport stream processor command-line options
//!
//----------------------------------------------------------------------------

#pragma once
#include "tsArgs.h"

namespace ts {
    //!
    //! Transport Stream Processor (tsp) namespace
    //!
    namespace tsp {
        //!
        //! Transport stream processor command-line options
        //! @ingroup plugin
        //!
        class Options: public Args
        {
        public:
            //!
            //! Constructor from command line options.
            //! @param [in] argc Number of arguments from command line.
            //! @param [in] argv Arguments from command line.
            //!
            Options(int argc, char *argv[]);

            //!
            //! Each plugin has one of the following types
            //!
            enum PluginType {
                INPUT,     //!< Input plugin.
                OUTPUT,    //!< Output plugin.
                PROCESSOR  //!< Packet processor plugin.
            };

            //!
            //! Displayable names of plugin types.
            //!
            static const Enumeration PluginTypeNames;

            //!
            //! Class containing the options for one plugin.
            //!
            struct PluginOptions
            {
                PluginType    type;  //!< Plugin type.
                UString       name;  //!< Plugin name.
                UStringVector args;  //!< Plugin options.

                //!
                //! Default constructor.
                //!
                PluginOptions();

                //!
                //! Display the content of this object to a stream.
                //! @param [in,out] strm Where to output the content.
                //! @param [in] indent Margin size, default: none.
                //! @return A reference to @a strm.
                //!
                std::ostream& display(std::ostream& strm, int indent = 0) const;
            };

            //!
            //! A vector of plugin options, representing the entire tsp processing chain.
            //!
            typedef std::vector<PluginOptions> PluginOptionsVector;

            // Option values
            bool          timed_log;       //!< Add time stamps in log messages.
            int           list_proc_flags; //!< List processors, mask of PluginRepository::ListFlag.
            bool          monitor;         //!< Run a resource monitoring thread.
            bool          ignore_jt;       //!< Ignore "joint termination" options in plugins.
            bool          sync_log;        //!< Synchronous log.
            size_t        bufsize;         //!< Buffer size.
            size_t        log_msg_count;   //!< Maximum buffered log messages.
            size_t        max_flush_pkt;   //!< Max processed packets before flush.
            size_t        max_input_pkt;   //!< Max packets per input operation.
            size_t        instuff_nullpkt; //!< Add input stuffing: add @a instuff_nullpkt null packets every @a instuff_inpkt input packets.
            size_t        instuff_inpkt;   //!< Add input stuffing: add @a instuff_nullpkt null packets every @a instuff_inpkt input packets.
            size_t        instuff_start;   //!< Add input stuffing: add @a instuff_start null packets before actual input.
            size_t        instuff_stop;    //!< Add input stuffing: add @a instuff_end null packets after end of actual input.
            BitRate       bitrate;         //!< Fixed input bitrate.
            MilliSecond   bitrate_adj;     //!< Bitrate adjust interval.
            Tristate      realtime;        //!< Use real-time options.
            PluginOptions input;           //!< Input plugin.
            PluginOptions output;          //!< Output plugin.
            PluginOptionsVector plugins;   //!< List of packet processor plugins.

            //!
            //! Apply default values to options which were not specified on the command line.
            //! @param [in] realtime If true, apply real-time defaults. If false, apply offline defaults.
            //!
            void applyDefaults(bool realtime);

            //!
            //! Display the content of this object to a stream.
            //! @param [in,out] strm Where to output the content.
            //! @param [in] indent Margin size, default: none.
            //! @return A reference to @a strm.
            //!
            std::ostream& display(std::ostream& strm, int indent = 0) const;

        private:
            Options() = delete;
            Options(const Options&) = delete;
            Options& operator=(const Options&) = delete;

            //!
            //! Options for -\-list-processor.
            //!
            static const Enumeration ListProcessorEnum;

            //!
            //! Search the next plugin option.
            //! @param [in] args Arguments from command line.
            //! @param [in] index Start searching at @a index.
            //! @param [out] type Plugin type.
            //! @return Index of plugin option or @a args.size() if not found.
            //!
            static size_t nextProcOpt(const UStringVector& args, size_t index, PluginType& type);
        };
    }
}

//!
//! Display operator for ts::tsp::Options.
//! @param [in,out] strm Where to output the content.
//! @param [in] opt The object to display.
//! @return A reference to @a strm.
//!
inline std::ostream& operator<<(std::ostream& strm, const ts::tsp::Options& opt)
{
    return opt.display(strm);
}
