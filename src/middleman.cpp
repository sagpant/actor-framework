/******************************************************************************\
 *           ___        __                                                    *
 *          /\_ \    __/\ \                                                   *
 *          \//\ \  /\_\ \ \____    ___   _____   _____      __               *
 *            \ \ \ \/\ \ \ '__`\  /'___\/\ '__`\/\ '__`\  /'__`\             *
 *             \_\ \_\ \ \ \ \L\ \/\ \__/\ \ \L\ \ \ \L\ \/\ \L\.\_           *
 *             /\____\\ \_\ \_,__/\ \____\\ \ ,__/\ \ ,__/\ \__/.\_\          *
 *             \/____/ \/_/\/___/  \/____/ \ \ \/  \ \ \/  \/__/\/_/          *
 *                                          \ \_\   \ \_\                     *
 *                                           \/_/    \/_/                     *
 *                                                                            *
 * Copyright (C) 2011, 2012                                                   *
 * Dominik Charousset <dominik.charousset@haw-hamburg.de>                     *
 *                                                                            *
 * This file is part of libcppa.                                              *
 * libcppa is free software: you can redistribute it and/or modify it under   *
 * the terms of the GNU Lesser General Public License as published by the     *
 * Free Software Foundation, either version 3 of the License                  *
 * or (at your option) any later version.                                     *
 *                                                                            *
 * libcppa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                       *
 * See the GNU Lesser General Public License for more details.                *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with libcppa. If not, see <http://www.gnu.org/licenses/>.            *
\******************************************************************************/


#include <tuple>
#include <cerrno>
#include <memory>
#include <cstring>
#include <sstream>
#include <iostream>
#include <stdexcept>

#include "cppa/on.hpp"
#include "cppa/actor.hpp"
#include "cppa/match.hpp"
#include "cppa/config.hpp"
#include "cppa/either.hpp"
#include "cppa/to_string.hpp"
#include "cppa/actor_proxy.hpp"
#include "cppa/binary_serializer.hpp"
#include "cppa/uniform_type_info.hpp"
#include "cppa/binary_deserializer.hpp"
#include "cppa/process_information.hpp"

#include "cppa/util/buffer.hpp"

#include "cppa/network/acceptor.hpp"
#include "cppa/network/middleman.hpp"
#include "cppa/network/input_stream.hpp"
#include "cppa/network/output_stream.hpp"
#include "cppa/network/default_protocol.hpp"
#include "cppa/network/addressed_message.hpp"

#include "cppa/detail/logging.hpp"
#include "cppa/detail/fd_util.hpp"
#include "cppa/detail/actor_registry.hpp"

#include "cppa/intrusive/single_reader_queue.hpp"

//  use epoll event-loop implementation on Linux if not explicitly overriden
//  by using CPPA_POLL_IMPL, use (default) poll implementation otherwise
#if defined(CPPA_LINUX) && !defined(CPPA_POLL_IMPL)
#   define CPPA_EPOLL_IMPL
#   include <sys/epoll.h>
#else
#   ifndef CPPA_POLL_IMPL
#       define CPPA_POLL_IMPL
#   endif
#   include <poll.h>
#endif

using namespace std;

namespace cppa { namespace network {

namespace {

const size_t ui32_size = sizeof(uint32_t);

template<class Container, class Element>
void erase_from(Container& haystack, const Element& needle) {
    typedef typename Container::value_type value_type;
    auto last = end(haystack);
    auto i = find_if(begin(haystack), last, [&](const value_type& value) {
        return value == needle;
    });
    if (i != last) haystack.erase(i);
}

template<class Container, class UnaryPredicate>
void erase_from_if(Container& container, const UnaryPredicate& predicate) {
    auto last = end(container);
    auto i = find_if(begin(container), last, predicate);
    if (i != last) container.erase(i);
}

class middleman_event {

    friend class intrusive::single_reader_queue<middleman_event>;

 public:

    template<typename Arg>
    middleman_event(Arg&& arg) : next(0), fun(forward<Arg>(arg)) { }

    inline void operator()() { fun(); }

 private:

    middleman_event* next;
    function<void()> fun;

};

typedef intrusive::single_reader_queue<middleman_event> middleman_queue;

} // namespace <anonymous>

class middleman_impl : public middleman {

    friend void middleman_loop(middleman_impl*);

 public:

    middleman_impl() {
        m_protocols.insert(make_pair(atom("DEFAULT"),
                                     new network::default_protocol(this)));
    }

    void add_protocol(const protocol_ptr& impl) {
        if (!impl) {
            CPPA_LOG_ERROR("impl == nullptr");
            throw std::invalid_argument("impl == nullptr");
        }
        CPPA_LOG_TRACE("identifier = " << to_string(impl->identifier()));
        std::lock_guard<util::shared_spinlock> guard(m_protocols_lock);
        m_protocols.insert(make_pair(impl->identifier(), impl));
    }

    protocol_ptr protocol(atom_value id) {
        util::shared_lock_guard<util::shared_spinlock> guard(m_protocols_lock);
        auto i = m_protocols.find(id);
        return (i != m_protocols.end()) ? i->second : nullptr;
    }

    void run_later(function<void()> fun) {
        CPPA_LOG_TRACE("");
        m_queue._push_back(new middleman_event(move(fun)));
        atomic_thread_fence(memory_order_seq_cst);
        uint8_t dummy = 0;
        if (write(m_pipe_write, &dummy, sizeof(dummy)) != sizeof(dummy)) {
            CPPA_CRITICAL("cannot write to pipe");
        }
    }

 protected:

    void start() {
        int pipefds[2];
        if (pipe(pipefds) != 0) { CPPA_CRITICAL("cannot create pipe"); }
        m_pipe_read = pipefds[0];
        m_pipe_write = pipefds[1];
        detail::fd_util::nonblocking(m_pipe_read, true);
        // start threads
        m_thread = thread([this] { middleman_loop(this); });
    }

    void stop() {
        run_later([this] { this->m_done = true; });
        //enqueue_message(middleman_message::create());
        m_thread.join();
        close(m_pipe_read);
        close(m_pipe_write);
    }

 private:

    thread m_thread;
    native_socket_type m_pipe_read;
    native_socket_type m_pipe_write;
    middleman_queue m_queue;

    util::shared_spinlock m_protocols_lock;
    map<atom_value,protocol_ptr> m_protocols;

};

middleman* middleman::create_singleton() {
    return new middleman_impl;
}

class middleman_overseer : public continuable_reader {

    typedef continuable_reader super;

 public:

    middleman_overseer(int pipe_fd, middleman_queue& q)
    : super(pipe_fd), m_queue(q) { }

    continue_reading_result continue_reading() {
        static constexpr size_t num_dummies = 64;
        uint8_t dummies[num_dummies];
        auto read_result = ::read(read_handle(), dummies, num_dummies);
        if (read_result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // try again later
                return read_continue_later;
            }
            else {
                CPPA_LOG_ERROR("cannot read from pipe");
                CPPA_CRITICAL("cannot read from pipe");
            }
        }
        atomic_thread_fence(memory_order_seq_cst);
        for (int i = 0; i < read_result; ++i) {
            unique_ptr<middleman_event> msg(m_queue.try_pop());
            if (!msg) {
                CPPA_LOG_ERROR("nullptr dequeued");
                CPPA_CRITICAL("nullptr dequeued");
            }
            (*msg)();
        }
        return read_continue_later;
    }

 private:

    middleman_queue& m_queue;

};

typedef int event_bitmask;

namespace event {

static constexpr event_bitmask none  = 0x00;
static constexpr event_bitmask read  = 0x01;
static constexpr event_bitmask write = 0x02;
static constexpr event_bitmask both  = 0x03;
static constexpr event_bitmask error = 0x04;

} // namespace event

inline const char* eb2str(event_bitmask e) {
    switch (e) {
        default: return "INVALID";
        case event::none:  return "event::none";
        case event::read:  return "event::read";
        case event::write: return "event::write";
        case event::both:  return "event::both";
        case event::error: return "event::error";
    }
}

struct fd_meta_info {
    native_socket_type fd;
    continuable_reader_ptr ptr;
    event_bitmask mask;
    fd_meta_info(native_socket_type a0,
                 const continuable_reader_ptr& a1,
                 event_bitmask a2)
    : fd(a0), ptr(a1), mask(a2) { }
};

enum class fd_meta_event { add, erase, mod };

struct fd_less {
    inline bool operator()(const fd_meta_info& lhs, native_socket_type rhs) const {
        return lhs.fd < rhs;
    }
};

class middleman_event_handler_base {

 public:

    typedef vector<fd_meta_info> vector_type;

    virtual ~middleman_event_handler_base() { }

    void alteration(const continuable_reader_ptr& ptr, event_bitmask e, fd_meta_event etype) {
        native_socket_type fd;
        switch (e) {
            case event::read:
                fd = ptr->read_handle();
                break;
            case event::write: {
                auto wptr = ptr->as_writer();
                if (wptr) fd = wptr->write_handle();
                else {
                    CPPA_LOG_ERROR("ptr->as_writer() returned nullptr");
                    return;
                }
                break;
            }
            case event::both: {
                fd = ptr->read_handle();
                auto wptr = ptr->as_writer();
                if (wptr) {
                    auto wrfd = wptr->write_handle();
                    if (fd != wrfd) {
                        CPPA_LOG_DEBUG("read_handle != write_handle, split "
                                       "into two function calls");
                        // split into two function calls
                        e = event::read;
                        alteration(ptr, event::write, etype);
                    }
                }
                else {
                    CPPA_LOG_ERROR("ptr->as_writer() returned nullptr");
                    return;
                }
                break;
            }
            default:
                CPPA_LOG_ERROR("invalid bitmask");
                return;
        }
        /*
        auto last = end(m_meta);
        auto iter = find_meta(fd);
        CPPA_LOG_ERROR_IF(iter == last && e == event::none,
                          "nothing to delete (no match)");
        event_bitmask old = (iter != last) ? iter->mask : event::none;
        auto next = cm(old, e);
        if (next != old) {
            m_alterations.emplace_back(fd, ptr, next);
        }
        */
        m_alterations.emplace_back(fd_meta_info(fd, ptr, e), etype);
    }

    void add(const continuable_reader_ptr& ptr, event_bitmask e) {
        CPPA_LOG_TRACE("ptr = " << ptr.get() << ", e = " << eb2str(e));
        alteration(ptr, e, fd_meta_event::add);
    }

    void erase(const continuable_reader_ptr& ptr, event_bitmask e) {
        CPPA_LOG_TRACE("ptr = " << ptr.get() << ", e = " << eb2str(e));
        alteration(ptr, e, fd_meta_event::erase);
    }

    inline event_bitmask next_bitmask(event_bitmask old, event_bitmask arg, fd_meta_event op) {
        CPPA_REQUIRE(op == fd_meta_event::add || op == fd_meta_event::erase);
        return (op == fd_meta_event::add) ? old | arg : old & ~arg;
    }

    void update() {
        CPPA_LOG_TRACE("");
        for (auto& elem_pair : m_alterations) {
            auto& elem = elem_pair.first;
            auto old = event::none;
            auto last = end(m_meta);
            auto iter = lower_bound(begin(m_meta), last, elem.fd, m_less);
            if (iter != last) old = iter->mask;
            auto mask = next_bitmask(old, elem.mask, elem_pair.second);
            auto ptr = elem.ptr.get();
            CPPA_LOG_DEBUG("new bitmask for "
                           << elem.ptr.get() << ": " << eb2str(mask));
            if (iter == last || iter->fd != elem.fd) {
                CPPA_LOG_INFO_IF(mask == event::none,
                                 "cannot erase " << ptr
                                 << " (not found in m_meta)");
                if (mask != event::none) {
                    m_meta.insert(iter, elem);
                    handle_event(fd_meta_event::add, elem.fd,
                                 event::none, mask, ptr);
                }
            }
            else if (iter->fd == elem.fd) {
                CPPA_REQUIRE(iter->ptr == elem.ptr);
                if (mask == event::none) {
                    m_meta.erase(iter);
                    handle_event(fd_meta_event::erase, elem.fd, old, mask, ptr);
                }
                else {
                    iter->mask = mask;
                    handle_event(fd_meta_event::mod, elem.fd, old, mask, ptr);
                }
            }
        }
        m_alterations.clear();
    }

 protected:

    virtual void handle_event(fd_meta_event me,
                              native_socket_type fd,
                              event_bitmask old_bitmask,
                              event_bitmask new_bitmask,
                              continuable_reader* ptr) = 0;

    fd_less m_less;
    vector_type m_meta; // this vector is *always* sorted

    vector<pair<fd_meta_info,fd_meta_event> > m_alterations;

 private:

    vector_type::iterator find_meta(native_socket_type fd) {
        auto last = end(m_meta);
        auto iter = lower_bound(begin(m_meta), last, fd, m_less);
        return (iter != last && iter->fd == fd) ? iter : last;
    }

};

template<class BaseIter, class BasIterAccess>
class event_iterator_impl {

 public:

    event_iterator_impl(const BaseIter& iter) : m_i(iter) { }

    inline event_iterator_impl& operator++() {
        m_access.advance(m_i);
        return *this;
    }

    inline event_iterator_impl* operator->() { return this; }

    inline const event_iterator_impl* operator->() const { return this; }

    inline event_bitmask type() const {
        return m_access.type(m_i);
    }

    inline continue_reading_result continue_reading() {
        return ptr()->continue_reading();
    }

    inline continue_writing_result continue_writing() {
        return ptr()->as_writer()->continue_writing();
    }

    inline bool equal_to(const event_iterator_impl& other) const {
        return m_access.equal(m_i, other.m_i);
    }

    inline void handled() { m_access.handled(m_i); }

    inline continuable_reader* ptr() { return m_access.ptr(m_i); }

 private:

    BaseIter m_i;
    BasIterAccess m_access;

};

template<class Iter, class Access>
inline bool operator==(const event_iterator_impl<Iter,Access>& lhs,
                       const event_iterator_impl<Iter,Access>& rhs) {
    return lhs.equal_to(rhs);
}

template<class Iter, class Access>
inline bool operator!=(const event_iterator_impl<Iter,Access>& lhs,
                       const event_iterator_impl<Iter,Access>& rhs) {
    return !lhs.equal_to(rhs);
}

#ifdef CPPA_POLL_IMPL

typedef pair<vector<pollfd>::iterator,vector<fd_meta_info>::iterator>
        pfd_iterator;

#ifndef POLLRDHUP
#define POLLRDHUP POLLHUP
#endif

struct pfd_access {

    inline void advance(pfd_iterator& i) const { ++(i.first); ++(i.second); }

    inline event_bitmask type(const pfd_iterator& i) const {
        auto revents = i.first->revents;
        if (revents == 0) return event::none;
        if (revents & (POLLRDHUP | POLLERR | POLLHUP | POLLNVAL)) {
            return event::error;
        }
        event_bitmask result = 0;
        if (revents & (POLLIN | POLLPRI)) result |= event::read;
        if (revents & POLLOUT) result |= event::write;
        CPPA_REQUIRE(result != 0);
        return result;
    }

    inline continuable_reader* ptr(pfd_iterator& i) const {
        return i.second->ptr.get();
    }

    inline bool equal(const pfd_iterator& lhs, const pfd_iterator& rhs) const {
        return lhs.first == rhs.first;
    }

    inline void handled(pfd_iterator& i) const { i.first->revents = 0; }

};

typedef event_iterator_impl<pfd_iterator,pfd_access> event_iterator;

struct pollfd_less {
    inline bool operator()(const pollfd& lhs, native_socket_type rhs) const {
        return lhs.fd < rhs;
    }
};

short to_poll_bitmask(event_bitmask mask) {
    switch (mask) {
        case event::read:  return POLLIN;
        case event::write: return POLLOUT;
        case event::both:  return (POLLIN|POLLOUT);
        default: CPPA_CRITICAL("invalid event bitmask");
    }
}

class middleman_event_handler : public middleman_event_handler_base {

 public:

    inline void init() { }

    size_t num_sockets() const { return m_pollset.size(); }

    pair<event_iterator,event_iterator> poll() {
        CPPA_REQUIRE(m_pollset.empty() == false);
        CPPA_REQUIRE(m_pollset.size() == m_meta.size());
        for (;;) {
            auto presult = ::poll(m_pollset.data(), m_pollset.size(), -1);
            CPPA_LOG_DEBUG("poll() on " << num_sockets()
                           << " sockets returned " << presult);
            if (presult < 0) {
                switch (errno) {
                    // a signal was caught
                    case EINTR: {
                        // just try again
                        break;
                    }
                    case ENOMEM: {
                        CPPA_LOG_ERROR("poll() failed for reason ENOMEM");
                        // there's not much we can do other than try again
                        // in hope someone releases memory
                        //this_thread::yield();
                        break;
                    }
                    default: {
                        perror("poll() failed");
                        CPPA_CRITICAL("poll() failed");
                    }
                }
            }
            else return {event_iterator({begin(m_pollset), begin(m_meta)}),
                         event_iterator({end(m_pollset), end(m_meta)})};
        }
    }

 protected:

    void handle_event(fd_meta_event me,
                      native_socket_type fd,
                      event_bitmask old_bitmask,
                      event_bitmask new_bitmask,
                      continuable_reader*) {
        static_cast<void>(old_bitmask); // no need for it
        switch (me) {
            case fd_meta_event::add: {
                pollfd tmp;
                tmp.fd = fd;
                tmp.events = to_poll_bitmask(new_bitmask);
                tmp.revents = 0;
                m_pollset.insert(lb(fd), tmp);
                CPPA_LOG_DEBUG("inserted new element");
                break;
            }
            case fd_meta_event::erase: {
                auto last = end(m_pollset);
                auto iter = lb(fd);
                CPPA_LOG_ERROR_IF(iter == last || iter->fd != fd,
                                  "m_meta and m_pollset out of sync; "
                                  "no element found for fd (cannot erase)");
                if (iter != last && iter->fd == fd) {
                    CPPA_LOG_DEBUG("erased element");
                    m_pollset.erase(iter);
                }
                break;
            }
            case fd_meta_event::mod: {
                auto last = end(m_pollset);
                auto iter = lb(fd);
                CPPA_LOG_ERROR_IF(iter == last || iter->fd != fd,
                                  "m_meta and m_pollset out of sync; "
                                  "no element found for fd (cannot erase)");
                if (iter != last && iter->fd == fd) {
                    CPPA_LOG_DEBUG("updated bitmask");
                    iter->events = to_poll_bitmask(new_bitmask);
                }
                break;
            }
        }
    }

 private:

    vector<pollfd>::iterator lb(native_socket_type fd) {
        return lower_bound(begin(m_pollset), end(m_pollset), fd, m_pless);
    }

    vector<pollfd> m_pollset; // always in sync with m_meta
    pollfd_less   m_pless;

};

#elif defined(CPPA_EPOLL_IMPL)

struct epoll_iterator_access {

    typedef vector<epoll_event>::iterator iterator;

    inline void advance(iterator& i) const { ++i; }

    inline event_bitmask type(const iterator& i) const {
        auto events = i->events;
        if (events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)) return event::error;
        auto result = event::none;
        if (events & EPOLLIN)  result |= event::read;
        if (events & EPOLLOUT) result |= event::write;
        CPPA_REQUIRE(result != 0);
        return result;
    }

    inline continuable_reader* ptr(iterator& i) const {
        return reinterpret_cast<continuable_reader*>(i->data.ptr);
    }

    inline bool equal(const iterator& lhs, const iterator& rhs) const {
        return lhs == rhs;
    }

    inline void handled(iterator&) const { }

};

typedef event_iterator_impl<vector<epoll_event>::iterator,epoll_iterator_access>
        event_iterator;

class middleman_event_handler : public middleman_event_handler_base {

 public:

    middleman_event_handler() : m_epollfd(-1) { }

    ~middleman_event_handler() { if (m_epollfd != -1) close(m_epollfd); }

    void init() {
        m_epollfd = epoll_create1(EPOLL_CLOEXEC);
        if (m_epollfd == -1) throw ios_base::failure(  string("epoll_create1: ")
                                                     + strerror(errno));
        // handle at most 64 events at a time
        m_events.resize(64);
    }

    size_t num_sockets() const { return m_meta.size(); }

    pair<event_iterator,event_iterator> poll() {
        CPPA_REQUIRE(m_meta.empty() == false);
        for (;;) {
            CPPA_LOG_DEBUG("epoll_wait on " << num_sockets() << " sockets");
            auto presult = epoll_wait(m_epollfd, m_events.data(),
                                      (int) m_events.size(), -1);
            CPPA_LOG_DEBUG("epoll_wait returned " << presult);
            if (presult < 0) {
                // try again unless critical error occured
                presult = 0;
                switch (errno) {
                    // a signal was caught
                    case EINTR: {
                        // just try again
                        break;
                    }
                    default: {
                        perror("epoll() failed");
                        CPPA_CRITICAL("epoll() failed");
                    }
                }
            }
            else {
                auto first = begin(m_events);
                auto last = first;
                advance(last, presult);
                return {first, last};
            }
        }
    }

 protected:


    void handle_event(fd_meta_event me,
                      native_socket_type fd,
                      event_bitmask old_bitmask,
                      event_bitmask new_bitmask,
                      continuable_reader* ptr) {
        static_cast<void>(old_bitmask); // no need for it
        int operation;
        epoll_event ee;
        ee.data.ptr = ptr;
        switch (new_bitmask) {
            case event::none:
                CPPA_REQUIRE(me == fd_meta_event::erase);
                ee.events = 0;
                break;
            case event::read:
                ee.events = EPOLLIN | EPOLLRDHUP;
                break;
            case event::write:
                ee.events = EPOLLOUT;
            case event::both:
                ee.events = EPOLLIN | EPOLLRDHUP | EPOLLOUT;
                break;
            default: CPPA_CRITICAL("invalid event bitmask");
        }
        switch (me) {
            case fd_meta_event::add:
                operation = EPOLL_CTL_ADD;
                break;
            case fd_meta_event::erase:
                operation = EPOLL_CTL_DEL;
                break;
            case fd_meta_event::mod:
                operation = EPOLL_CTL_MOD;
                break;
            default: CPPA_CRITICAL("invalid fd_meta_event");
        }
        if (epoll_ctl(m_epollfd, operation, fd, &ee) < 0) {
            switch (errno) {
                // supplied file descriptor is already registered
                case EEXIST: {
                    CPPA_LOG_ERROR("file descriptor registered twice");
                    break;
                }
                // op was EPOLL_CTL_MOD or EPOLL_CTL_DEL,
                // and fd is not registered with this epoll instance.
                case ENOENT: {
                    CPPA_LOG_ERROR("cannot delete file descriptor "
                                   "because it isn't registered");
                    break;
                }
                default: {
                    CPPA_LOG_ERROR(strerror(errno));
                    perror("epoll_ctl() failed");
                    CPPA_CRITICAL("epoll_ctl() failed");
                }
            }
        }
    }

    int m_epollfd;
    vector<epoll_event> m_events;

};

#endif

middleman::middleman() : m_done(false), m_handler(new middleman_event_handler){}

middleman::~middleman() { }

void middleman::continue_writer(const continuable_reader_ptr& ptr) {
    CPPA_LOG_TRACE("ptr = " << ptr.get());
    CPPA_REQUIRE(ptr->as_writer() != nullptr);
    m_handler->add(ptr, event::write);
}

void middleman::stop_writer(const continuable_reader_ptr& ptr) {
    CPPA_LOG_TRACE("ptr = " << ptr.get());
    CPPA_REQUIRE(ptr->as_writer() != nullptr);
    m_handler->erase(ptr, event::write);
}

void middleman::continue_reader(const continuable_reader_ptr& ptr) {
    CPPA_LOG_TRACE("ptr = " << ptr.get());
    m_readers.push_back(ptr);
    m_handler->add(ptr, event::read);
}

void middleman::stop_reader(const continuable_reader_ptr& ptr) {
    CPPA_LOG_TRACE("ptr = " << ptr.get());
    m_handler->erase(ptr, event::read);
    erase_from(m_readers, ptr);
}

void middleman_loop(middleman_impl* impl) {
    middleman_event_handler* handler = impl->m_handler.get();
    CPPA_LOGF_TRACE("run middleman loop, node: "
                    << to_string(*process_information::get()));
    handler->init();
    impl->continue_reader(new middleman_overseer(impl->m_pipe_read, impl->m_queue));
    handler->update();
    while (!impl->done()) {
        auto iters = handler->poll();
        for (auto i = iters.first; i != iters.second; ++i) {
            auto mask = i->type();
            switch (mask) {
                default: CPPA_CRITICAL("invalid event");
                case event::none: break;
                case event::both:
                case event::write: {
                    CPPA_LOGF_DEBUG("handle event::write");
                    switch (i->continue_writing()) {
                        case write_closed:
                        case write_failure:
                            impl->stop_writer(i->ptr());
                            CPPA_LOGF_DEBUG("writer removed because of error");
                            break;
                        case write_done:
                            impl->stop_writer(i->ptr());
                            break;
                        default: break;
                    }
                    if (mask == event::write) break;
                    // else: fall through
                    CPPA_LOGF_DEBUG("handle event::both; fall through");
                }
                case event::read: {
                    CPPA_LOGF_DEBUG("handle event::read");
                    switch (i->continue_reading()) {
                        case read_closed:
                        case read_failure:
                            impl->stop_reader(i->ptr());
                            CPPA_LOGF_DEBUG("remove peer");
                            break;
                        default: break;
                    }
                    break;
                }
                case event::error: {
                    impl->stop_reader(i->ptr());
                    impl->stop_writer(i->ptr());
                    CPPA_LOGF_DEBUG("event::error; remove peer");
                }
            }
            i->handled();
        }
        handler->update();
    }
    CPPA_LOGF_DEBUG("event loop done, erase all readers");
    // make sure to write everything before shutting down
    for (auto ptr : impl->m_readers) { handler->erase(ptr, event::read); }
    handler->update();
    CPPA_LOGF_DEBUG("flush outgoing messages");
    CPPA_LOGF_DEBUG_IF(handler->num_sockets() == 0,
                       "nothing to flush, no writer left");
    while (handler->num_sockets() > 0) {
        auto iters = handler->poll();
        for (auto i = iters.first; i != iters.second; ++i) {
            switch (i->type()) {
                case event::write:
                    switch (i->continue_writing()) {
                        case write_closed:
                        case write_failure:
                        case write_done:
                            handler->erase(i->ptr(), event::write);
                            break;
                        default: break;
                    }
                    break;
                case event::error:
                    handler->erase(i->ptr(), event::both);
                    break;
                default:
                    CPPA_LOGF_ERROR("expected event::write only "
                                    "during shutdown phase");
                    handler->erase(i->ptr(), event::read);
                    break;
            }
        }
        handler->update();
    }
    CPPA_LOGF_DEBUG("clear all containers");
    //impl->m_peers.clear();
    impl->m_readers.clear();
    CPPA_LOGF_DEBUG("middleman loop done");
}

} } // namespace cppa::detail
