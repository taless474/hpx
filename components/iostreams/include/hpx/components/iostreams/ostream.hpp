//  Copyright (c) 2011 Bryce Lelbach
//  Copyright (c) 2011-2020 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <hpx/config.hpp>

#include <hpx/async_distributed/apply.hpp>
#include <hpx/modules/async_distributed.hpp>
#include <hpx/execution_base/register_locks.hpp>
#include <hpx/components/iostreams/manipulators.hpp>
#include <hpx/components/iostreams/server/output_stream.hpp>
#include <hpx/runtime/components/client_base.hpp>

#include <boost/iostreams/stream.hpp>

#include <atomic>
#include <cstdint>
#include <ios>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hpx { namespace iostreams
{
    ///////////////////////////////////////////////////////////////////////////
    namespace detail
    {
        template <typename Char = char>
        struct buffer_sink;
    }

    template <typename Char = char, typename Sink = detail::buffer_sink<Char> >
    struct ostream;

    ///////////////////////////////////////////////////////////////////////////
    namespace detail
    {
        ///////////////////////////////////////////////////////////////////////
        // Tag types to be used to identify standard ostream objects
        struct cout_tag {};
        struct cerr_tag {};

        struct consolestream_tag {};

        ///////////////////////////////////////////////////////////////////////
        /// This is a Boost.IoStreams Sink that can be used to create an
        /// [io]stream on top of a detail::buffer.
        template <typename Char>
        struct buffer_sink
        {
            typedef Char char_type;

            struct category
              : boost::iostreams::sink_tag,
                boost::iostreams::flushable_tag
            {};

            explicit buffer_sink(ostream<Char, buffer_sink>& os)
              : os_(os)
            {}

            // Write up to n characters to the underlying data sink into the
            // buffer s, returning the number of characters written.
            inline std::streamsize write(char_type const* s, std::streamsize n);

            // Make sure all content is sent to console
            inline bool flush();

        private:
            ostream<Char, buffer_sink>& os_;
        };

        ///////////////////////////////////////////////////////////////////////
        template <typename Char = char, typename Sink = buffer_sink<Char> >
        struct ostream_creator
        {
            typedef std::back_insert_iterator<std::vector<Char> > iterator_type;
            typedef Sink device_type;
            typedef boost::iostreams::stream<device_type> stream_type;
        };

        ///////////////////////////////////////////////////////////////////////
        inline std::ostream& get_outstream(cout_tag)
        {
            return std::cout;
        }

        inline std::ostream& get_outstream(cerr_tag)
        {
            return std::cerr;
        }

        std::stringstream& get_consolestream();

        inline std::ostream& get_outstream(consolestream_tag)
        {
            return get_consolestream();
        }

        inline char const* get_outstream_name(cout_tag)
        {
            return "/locality#console/output_stream#cout";
        }

        inline char const* get_outstream_name(cerr_tag)
        {
            return "/locality#console/output_stream#cerr";
        }

        inline char const* get_outstream_name(consolestream_tag)
        {
            return "/locality#console/output_stream#consolestream";
        }

        ///////////////////////////////////////////////////////////////////////
        hpx::future<naming::id_type>
        create_ostream(char const* name, std::ostream& strm);

        template <typename Tag>
        hpx::future<naming::id_type> create_ostream(Tag tag)
        {
            return create_ostream(get_outstream_name(tag),
                detail::get_outstream(tag));
        }

        ///////////////////////////////////////////////////////////////////////
        HPX_IOSTREAMS_EXPORT void release_ostream(
            char const* name, naming::id_type const& id);

        template <typename Tag>
        void release_ostream(Tag tag, naming::id_type const& id)
        {
            release_ostream(get_outstream_name(tag), id);
        }

        ///////////////////////////////////////////////////////////////////////
        void register_ostreams();
        void unregister_ostreams();
    }

    ///////////////////////////////////////////////////////////////////////////
    template <typename Char, typename Sink>
    struct ostream
        : components::client_base<ostream<Char, Sink>, server::output_stream>
        , detail::buffer
        , detail::ostream_creator<Char, Sink>::stream_type
    {
        HPX_NON_COPYABLE(ostream);

    private:
        typedef components::client_base<ostream, server::output_stream> base_type;

        typedef detail::ostream_creator<Char, Sink> ostream_creator;
        typedef typename ostream_creator::stream_type stream_base_type;
        typedef typename ostream_creator::iterator_type iterator_type;

        typedef typename stream_base_type::traits_type stream_traits_type;
        typedef BOOST_IOSTREAMS_BASIC_OSTREAM(Char, stream_traits_type) std_stream_type;
        typedef detail::buffer::mutex_type mutex_type;

    private:
        using detail::buffer::mtx_;
        std::atomic<std::uint64_t> generational_count_;

        // Performs a lazy streaming operation.
        template <typename T>
        ostream& streaming_operator_lazy(T const& subject)
        { // {{{
            // apply the subject to the local stream
            *static_cast<stream_base_type*>(this) << subject;
            return *this;
        } // }}}

        // Performs an asynchronous streaming operation.
        template <typename T, typename Lock>
        ostream& streaming_operator_async(T const& subject, Lock& l)
        { // {{{
            // apply the subject to the local stream
            *static_cast<stream_base_type*>(this) << subject;

            // If the buffer isn't empty, send it asynchronously to the
            // destination.
            if (!this->detail::buffer::empty_locked())
            {
                // Create the next buffer, returns the previous buffer
                buffer next = this->detail::buffer::init_locked();

                // Unlock the mutex before we cleanup.
                l.unlock();

                // Perform the write operation, then destroy the old buffer and
                // stream.
                typedef server::output_stream::write_async_action action_type;
                hpx::apply<action_type>(this->get_id(), hpx::get_locality_id(),
                    generational_count_++, next);
            }

            return *this;
        } // }}}

        // Performs a synchronous streaming operation.
        template <typename T, typename Lock>
        ostream& streaming_operator_sync(T const& subject, Lock& l)
        { // {{{
            // apply the subject to the local stream
            *static_cast<stream_base_type*>(this) << subject;

            // Send even empty buffer to flush the data buffered server-side.

            // Create the next buffer, returns the previous buffer
            buffer next = this->detail::buffer::init_locked();

            // Unlock the mutex before we cleanup.
            l.unlock();

            // Perform the write operation, then destroy the old buffer and
            // stream.
            typedef server::output_stream::write_sync_action action_type;
            hpx::async<action_type>(this->get_id(), hpx::get_locality_id(),
                generational_count_++, next).get();

            return *this;
        } // }}}

        ///////////////////////////////////////////////////////////////////////
        friend struct detail::buffer_sink<char>;

        bool flush()
        {
            std::unique_lock<mutex_type> l(*mtx_);
            if (!this->detail::buffer::empty_locked())
            {
                // Create the next buffer, returns the previous buffer
                buffer next = this->detail::buffer::init_locked();

                // Unlock the mutex before we cleanup.
                l.unlock();

                // since mtx_ is recursive and apply will do an AGAS lookup,
                // we need to ignore the lock here in case we are called
                // recursively
                hpx::util::ignore_while_checking<std::unique_lock<mutex_type> >
                    il(&l);

                // Perform the write operation, then destroy the old buffer and
                // stream.
                typedef server::output_stream::write_async_action action_type;
                hpx::apply<action_type>(this->get_id(), hpx::get_locality_id(),
                    generational_count_++, next);
            }
            return true;
        }

        ///////////////////////////////////////////////////////////////////////
        friend void detail::register_ostreams();
        friend void detail::unregister_ostreams();

        // late initialization during runtime system startup
        template <typename Tag>
        void initialize(Tag tag)
        {
            *static_cast<base_type*>(this) = detail::create_ostream(tag);
        }

        // reset this object during runtime system shutdown
        template <typename Tag>
        void uninitialize(Tag tag)
        {
            std::unique_lock<mutex_type> l(*mtx_, std::try_to_lock);
            if (l)
            {
                streaming_operator_sync(hpx::async_flush, l);   // unlocks
            }

            // FIXME: find a later spot to invoke this
            detail::release_ostream(tag, this->get_id());
            this->base_type::free();
        }

    public:
        ostream()
          : base_type()
          , buffer()
          , stream_base_type(*this)
          , generational_count_(0)
        {}

        // hpx::flush manipulator
        ostream& operator<<(hpx::iostreams::flush_type const& m)
        {
            std::unique_lock<mutex_type> l(*mtx_);
            return streaming_operator_sync(m, l);
        }

        // hpx::endl manipulator
        ostream& operator<<(hpx::iostreams::endl_type const& m)
        {
            std::unique_lock<mutex_type> l(*mtx_);
            return streaming_operator_sync(m, l);
        }

        // hpx::async_flush manipulator
        ostream& operator<<(hpx::iostreams::async_flush_type const& m)
        {
            std::unique_lock<mutex_type> l(*mtx_);
            return streaming_operator_async(m, l);
        }

        // hpx::async_endl manipulator
        ostream& operator<<(hpx::iostreams::async_endl_type const& m)
        {
            std::unique_lock<mutex_type> l(*mtx_);
            return streaming_operator_async(m, l);
        }

        ///////////////////////////////////////////////////////////////////////
        template <typename T>
        ostream& operator<<(T const& subject)
        {
            std::lock_guard<mutex_type> l(*mtx_);
            return streaming_operator_lazy(subject);
        }

        ///////////////////////////////////////////////////////////////////////
        ostream& operator<<(std_stream_type& (*manip_fun)(std_stream_type&))
        {
            std::unique_lock<mutex_type> l(*mtx_);
            util::ignore_while_checking<std::unique_lock<mutex_type> > ignore(&l);
            return streaming_operator_lazy(manip_fun);
        }
    };

    ///////////////////////////////////////////////////////////////////////////
    namespace detail
    {
        template <typename Char>
        inline std::streamsize buffer_sink<Char>::write(
            Char const* s, std::streamsize n)
        {
            return static_cast<buffer&>(os_).write(s, n);
        }

        template <typename Char>
        inline bool buffer_sink<Char>::flush()
        {
            return os_.flush();
        }
    }
}}

namespace hpx { namespace util {
    // TODO: This overload should not be needed. See #3175.
    template <typename Char, typename Sink, typename... Args>
    hpx::iostreams::ostream<Char, Sink>& format_to(
        hpx::iostreams::ostream<Char, Sink>& os, std::string const& format_str,
        Args const&... args)
    {
        return os << format(format_str, args...);
    }
}}


