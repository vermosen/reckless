#define NDEBUG
#include "dlog.hpp"

#include <memory>
#include <algorithm>
#include <new>
#include <thread>
#include <deque>

#include <sstream> // TODO probably won't need this when all is said and done
#include <iostream>

#include <cstring>
#include <cassert>
#include <cstdio>
#include <cmath>

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

namespace {
    int const g_page_size = static_cast<int>(sysconf(_SC_PAGESIZE));
}

namespace dlog {
    namespace detail {
        void output_worker();

#ifdef ASYNCLOG_USE_CXX11_THREAD_LOCAL
        thread_local input_buffer tls_input_buffer; 
        thread_local input_buffer* tls_pinput_buffer; 
#else
        input_buffer* tls_pinput_buffer; 
#endif
        std::size_t const TLS_INPUT_BUFFER_SIZE = 8*4096;   // static

        shared_input_queue_t g_shared_input_queue;
        spsc_event g_shared_input_consumed_event;
        spsc_event g_shared_input_queue_full_event;

        std::unique_ptr<output_buffer> g_poutput_buffer;
        std::thread g_output_thread;
    }
}

void dlog::initialize(writer* pwriter)
{
    initialize(pwriter, 0, 0);
}

void dlog::initialize(writer* pwriter, std::size_t max_output_buffer_size)
{
    using namespace detail;

    if(max_output_buffer_size == 0)
        max_output_buffer_size = 1024*1024;

    g_poutput_buffer.reset(new output_buffer(pwriter, max_output_buffer_size));

    g_output_thread = move(std::thread(&output_worker));
}

// TODO dlog::crash_cleanup or panic_cleanup or similar, just dump everything
// to disk in the event of a crash.
void dlog::cleanup()
{
    using namespace detail;
    commit();
    queue_commit_extent({nullptr, 0});
    g_output_thread.join();
    assert(g_input_queue.empty());
    g_poutput_buffer.reset();
}

dlog::writer::~writer()
{
}

dlog::file_writer::file_writer(char const* path) :
    fd_(-1)
{
    auto full_access =
        S_IRUSR | S_IWUSR | S_IXUSR |
        S_IRGRP | S_IWGRP | S_IXGRP |
        S_IROTH | S_IWOTH | S_IXOTH;
    fd_ = open(path, O_WRONLY | O_CREAT, full_access);
    // TODO proper exception here.
    if(fd_ == -1)
        throw std::runtime_error("cannot open file");
    lseek(fd_, 0, SEEK_END);
}

dlog::file_writer::~file_writer()
{
    if(fd_ != -1)
        close(fd_);
}

auto dlog::file_writer::write(void const* pbuffer, std::size_t count) -> Result
{
    char const* p = static_cast<char const*>(pbuffer);
    while(count != 0) {
        ssize_t written = ::write(fd_, p, count);
        if(written == -1) {
            if(errno != EINTR)
                break;
        } else {
            p += written;
            count -= written;
        }
    }
    if(count == 0)
        return SUCCESS;

    // TODO handle broken pipe signal?
    switch(errno) {
    case EFBIG:
    case EIO:
    case EPIPE:
    case ERANGE:
    case ECONNRESET:
    case EINVAL:
    case ENXIO:
    case EACCES:
    case ENETDOWN:
    case ENETUNREACH:
        // TODO handle this error by not writing to the buffer any more.
        return ERROR_GIVE_UP;
    case ENOSPC:
        return ERROR_TRY_LATER;
    default:
        // TODO throw proper error
        throw std::runtime_error("cannot write to file descriptor");
    }
}

dlog::output_buffer::output_buffer(writer* pwriter, std::size_t max_capacity) :
    pwriter_(pwriter),
    pbuffer_(nullptr),
    pcommit_end_(nullptr),
    pbuffer_end_(nullptr)
{
    using namespace detail;
    pbuffer_ = static_cast<char*>(malloc(max_capacity));
    pcommit_end_ = pbuffer_;
    pbuffer_end_ = pbuffer_ + max_capacity;
    madvise(pbuffer_ + g_page_size, max_capacity - g_page_size, MADV_DONTNEED);
}

dlog::output_buffer::~output_buffer()
{
    free(pbuffer_);
}

char* dlog::output_buffer::reserve(std::size_t size)
{
    if(pbuffer_end_ - pcommit_end_ < size) {
        flush();
        // TODO if the flush fails above, the only thing we can do is discard
        // the data. But perhaps we should invoke a callback that can do
        // something, such as log a message about the discarded data.
        if(pbuffer_end_ - pbuffer_ < size)
            throw std::bad_alloc();
    }
    return pcommit_end_;
}

void dlog::output_buffer::flush()
{
    // TODO keep track of a high watermark, i.e. max value of pcommit_end_.
    // Clear every second or some such. Use madvise to release unused memory.
    pwriter_->write(pbuffer_, pcommit_end_ - pbuffer_);
    pcommit_end_ = pbuffer_;
}

namespace dlog {
namespace {
    template <typename T>
    bool generic_format_int(output_buffer* pbuffer, char const*& pformat, T v)
    {
        char f = *pformat;
        if(f == 'd') {
            std::ostringstream ostr;
            ostr << v;
            std::string const& s = ostr.str();
            char* p = pbuffer->reserve(s.size());
            std::memcpy(p, s.data(), s.size());
            pbuffer->commit(s.size());
            pformat += 1;
            return true;
        } else if(f == 'x') {
            // FIXME
            return false;
        } else if(f == 'b') {
            // FIXME
            return false;
        } else {
            return false;
        }
    }

    int typesafe_sprintf(char* str, double v)
    {
        return sprintf(str, "%f", v);
    }

    int typesafe_sprintf(char* str, long double v)
    {
        return sprintf(str, "%Lf", v);
    }

    template <typename T>
    bool generic_format_float(output_buffer* pbuffer, char const*& pformat, T v)
    {
        char f = *pformat;
        if(f != 'd')
            return false;

        T v_for_counting_digits = std::fabs(v);
        if(v_for_counting_digits < 1.0)
            v_for_counting_digits = static_cast<T>(1.0);
        std::size_t digits = static_cast<std::size_t>(std::log10(v_for_counting_digits)) + 1;
        // %f without precision modifier gives us 6 digits after the decimal point.
        // The format is [-]ddd.dddddd (minimum 9 chars). We can also get e.g.
        // "-infinity" but that won't be more chars than the digits.
        char* p = pbuffer->reserve(1+digits+1+6+1);
        int written = typesafe_sprintf(p, v);
        pbuffer->commit(written);
        pformat += 1;
        return true;
    }

    template <typename T>
    bool generic_format_char(output_buffer* pbuffer, char const*& pformat, T v)
    {
        char f = *pformat;
        if(f == 's') {
            char* p = pbuffer->reserve(1);
            *p = static_cast<char>(v);
            pbuffer->commit(1);
            pformat += 1;
            return true;
        } else {
            return generic_format_int(pbuffer, pformat, static_cast<int>(v));
        }
    }
}   // anonymous namespace
}   // namespace dlog

bool dlog::format(output_buffer* pbuffer, char const*& pformat, char v)
{
    return generic_format_char(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, signed char v)
{
    return generic_format_char(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, unsigned char v)
{
    return generic_format_char(pbuffer, pformat, v);
}

//bool dlog::format(output_buffer* pbuffer, char const*& pformat, wchar_t v);
//bool dlog::format(output_buffer* pbuffer, char const*& pformat, char16_t v);
//bool dlog::format(output_buffer* pbuffer, char const*& pformat, char32_t v);

bool dlog::format(output_buffer* pbuffer, char const*& pformat, short v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, unsigned short v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, int v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, unsigned int v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, long v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, unsigned long v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, long long v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, unsigned long long v)
{
    return generic_format_int(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, float v)
{
    return generic_format_float(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, double v)
{
    return generic_format_float(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, long double v)
{
    return generic_format_float(pbuffer, pformat, v);
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, char const* v)
{
    if(*pformat != 's')
        return false;
    auto len = std::strlen(v);
    char* p = pbuffer->reserve(len);
    std::memcpy(p, v, len);
    pbuffer->commit(len);
    ++pformat;
    return true;
}

bool dlog::format(output_buffer* pbuffer, char const*& pformat, std::string const& v)
{
    if(*pformat != 's')
        return false;
    auto len = v.size();
    char* p = pbuffer->reserve(len);
    std::memcpy(p, v.data(), len);
    pbuffer->commit(len);
    ++pformat;
    return true;
}

dlog::detail::input_buffer::input_buffer() :
    pbegin_(allocate_buffer())
{
    pinput_start_ = pbegin_;
    pinput_end_ = pbegin_;
    pcommit_end_ = pbegin_;
}

dlog::detail::input_buffer::~input_buffer()
{
    commit();
    // Both commit() and wait_input_consumed should create full memory barriers,
    // so no need for strict memory ordering in this load.
    while(pinput_start_.load(std::memory_order_relaxed) != pinput_end_)
        wait_input_consumed();

    free(pbegin_);
}

// Helper for allocating aligned ring buffer in ctor.
char* dlog::detail::input_buffer::allocate_buffer()
{
    void* pbuffer = nullptr;
    // TODO proper error here. bad_alloc?
    if(0 != posix_memalign(&pbuffer, FRAME_ALIGNMENT, TLS_INPUT_BUFFER_SIZE))
        throw std::runtime_error("cannot allocate input frame");
    *static_cast<char*>(pbuffer) = 'X';
    return static_cast<char*>(pbuffer);
}


// Moves an input-buffer pointer forward by the given distance while
// maintaining the invariant that:
//
// * p is aligned by FRAME_ALIGNMENT
// * p never points at the end of the buffer; it always wraps around to the
//   beginning of the circular buffer.
//
// The distance must never be so great that the pointer moves *past* the end of
// the buffer. To do so would be an error in our context, since no input frame
// is allowed to be discontinuous.
char* dlog::detail::input_buffer::advance_frame_pointer(char* p, std::size_t distance)
{
    assert(is_aligned(distance));
    //p = align(p + distance, FRAME_ALIGNMENT);
    p += distance;
    auto remaining = TLS_INPUT_BUFFER_SIZE - (p - pbegin_);
    assert(remaining >= 0);
    if(remaining == 0)
        p = pbegin_;
    return p;
}

char* dlog::detail::input_buffer::allocate_input_frame(std::size_t size)
{
    // Conceptually, we have the invariant that
    //   pinput_start <= pinput_end,
    // and the memory area after pinput_end is free for us to use for
    // allocating a frame. However, the fact that it's a circular buffer means
    // that:
    // 
    // * The area after pinput_end is actually non-contiguous, wraps around
    //   at the end of the buffer and ends at pinput_start.
    //   
    // * Except, when pinput_end itself has fallen over the right edge and we
    //   have the case pinput_end <= pinput_start. Then the *used* memory is
    //   non-contiguous, and the free memory is contiguous (it still starts at
    //   pinput_end and ends at pinput_start modulo circular buffer size).
    //   
    // (This is easier to understand by drawing it on a paper than by reading
    // comment text).
    while(true) {
        auto pinput_end = pinput_end_;
        assert(pinput_end - pbegin_ != TLS_INPUT_BUFFER_SIZE);
        assert(is_aligned(pinput_end, FRAME_ALIGNMENT));

        // Even if we get an "old" value for pinput_start_ here, that's OK
        // because other threads will never cause the amount of available
        // buffer space to shrink. So either there is enough buffer space and
        // we're done, or there isn't and we'll wait for an input-consumption
        // event which creates a full memory barrier and hence gives us an
        // updated value for pinput_start_. So memory_order_relaxed should be
        // fine here.
        auto pinput_start = pinput_start_.load(std::memory_order_relaxed);
        //std::cout << "W " <<  std::hex << (pinput_start - pbegin_) << " - " << std::hex << (pinput_end - pbegin_) << std::endl;
        auto free = pinput_start - pinput_end;
        if(free > 0) {
            // Free space is contiguous.
            // Technically, there is enough room if size == free. But the
            // problem with using the free space in this situation is that when
            // we increase pinput_end_ by size, we end up with pinput_start_ ==
            // pinput_end_. Now, given that state, how do we know if the buffer
            // is completely filled or empty? So, it's easier to just check for
            // size < free instead of size <= free, and call pretend we're out
            // of space if size == free. Same situation applies in the else
            // clause below.
            if(likely(size < free)) {
                pinput_end_ = advance_frame_pointer(pinput_end, size);
                return pinput_end;
            } else {
                // Not enough room. Wait for the output thread to consume some
                // input.
                wait_input_consumed();
            }
        } else {
            // Free space is non-contiguous.
            std::size_t free1 = TLS_INPUT_BUFFER_SIZE - (pinput_end - pbegin_);
            if(likely(size < free1)) {
                // There's enough room in the first segment.
                pinput_end_ = advance_frame_pointer(pinput_end, size);
                return pinput_end;
            } else {
                std::size_t free2 = pinput_start - pbegin_;
                if(likely(size < free2)) {
                    // We don't have enough room for a continuous input frame
                    // in the first segment (at the end of the circular
                    // buffer), but there is enough room in the second segment
                    // (at the beginning of the buffer). To instruct the output
                    // thread to skip ahead to the second segment, we need to
                    // put a marker value at the current position. We're
                    // supposed to be guaranteed enough room for the wraparound
                    // marker because FRAME_ALIGNMENT is at least the size of
                    // the marker.
                    *reinterpret_cast<dispatch_function_t**>(pinput_end_) =
                        WRAPAROUND_MARKER;
                    pinput_end_ = advance_frame_pointer(pbegin_, size);
                    return pbegin_;
                } else {
                    // Not enough room. Wait for the output thread to consume
                    // some input.
                    wait_input_consumed();
                }
            }
        }
    }
}

char* dlog::detail::input_buffer::input_start() const
{
    return pinput_start_.load(std::memory_order_relaxed);
}

char* dlog::detail::input_buffer::discard_input_frame(std::size_t size)
{
    // We can use relaxed memory ordering everywhere here because there is
    // nothing being written of interest that the pointer update makes visible;
    // all it does is *discard* data, not provide any new data (besides,
    // signaling the event is likely to create a full memory barrier anyway).
    auto p = pinput_start_.load(std::memory_order_relaxed);
    p = advance_frame_pointer(p, size);
    pinput_start_.store(p, std::memory_order_relaxed);
    signal_input_consumed();
    return p;
}

char* dlog::detail::input_buffer::wraparound()
{
#ifndef NDEBUG
    auto p = pinput_start_.load(std::memory_order_relaxed);
    auto marker = *reinterpret_cast<dispatch_function_t**>(p);
    assert(WRAPAROUND_MARKER == marker);
#endif
    pinput_start_.store(pbegin_, std::memory_order_relaxed);
    return pbegin_;
}

void dlog::detail::input_buffer::signal_input_consumed()
{
    input_consumed_event_.signal();
}

void dlog::detail::input_buffer::wait_input_consumed()
{
    // This is kind of icky, we need to lock a mutex just because the condition
    // variable requires it. There would be less overhead if we could just use
    // something like Windows event objects.
    if(pcommit_end_ == pinput_start_.load(std::memory_order_relaxed)) {
        // We are waiting for input to be consumed because the input buffer is
        // full, but we haven't actually posted any data (i.e. we haven't
        // called commit). In other words, the caller has written too much to
        // the log without committing. The best effort we can make is to commit
        // whatever we have so far, otherwise the wait below will block
        // forever.
        commit();
    }
    // FIXME we need to think about what to do here, should we signal
    // g_shared_input_queue_full_event to force the output thread to wake up?
    // We probably should, or we could sit here for a full second.
    input_consumed_event_.wait();
}
void dlog::detail::output_worker()
{
    using namespace detail;
    while(true) {
        commit_extent ce;
        unsigned wait_time_ms = 0;
        while(not g_shared_input_queue.pop(ce)) {
            g_shared_input_queue_full_event.wait(wait_time_ms);
            wait_time_ms = (wait_time_ms == 0)? 1 : 2*wait_time_ms;
            wait_time_ms = std::min(wait_time_ms, 1000u);
        }
        g_shared_input_consumed_event.signal();
            
        if(not ce.pinput_buffer)
            // Request to shut down thread.
            return;

        char* pinput_start = ce.pinput_buffer->input_start();
        while(pinput_start != ce.pcommit_end) {
            //std::cout << "R " <<  std::hex << (pinput_start - ce.pinput_buffer->pbegin_) << std::endl;
            auto pdispatch = *reinterpret_cast<dispatch_function_t**>(pinput_start);
            if(WRAPAROUND_MARKER == pdispatch) {
                pinput_start = ce.pinput_buffer->wraparound();
                pdispatch = *reinterpret_cast<dispatch_function_t**>(pinput_start);
            }
            auto frame_size = (*pdispatch)(g_poutput_buffer.get(), pinput_start);
            pinput_start = ce.pinput_buffer->discard_input_frame(frame_size);
        }
        // TODO we *could* do something like flush on a timer instead when we're getting a lot of writes / sec.
        // OR, we should at least keep on dumping data without flush as long as the input queue has data to give us.
        g_poutput_buffer->flush();
    }
}


void dlog::formatter::format(output_buffer* pbuffer, char const* pformat)
{
    auto len = std::strlen(pformat);
    char* p = pbuffer->reserve(len);
    std::memcpy(p, pformat, len);
    pbuffer->commit(len);
}

void dlog::formatter::append_percent(output_buffer* pbuffer)
{
    auto p = pbuffer->reserve(1u);
    *p = '%';
    pbuffer->commit(1u);
}

char const* dlog::formatter::next_specifier(output_buffer* pbuffer,
        char const* pformat)
{
    while(true) {
        char const* pspecifier = std::strchr(pformat, '%');
        if(pspecifier == nullptr) {
            format(pbuffer, pformat);
            return nullptr;
        }

        auto len = pspecifier - pformat;
        auto p = pbuffer->reserve(len);
        std::memcpy(p, pformat, len);
        pbuffer->commit(len);

        pformat = pspecifier + 1;

        if(*pformat != '%')
            return pformat;

        ++pformat;
        append_percent(pbuffer);
    }
}

void dlog::detail::queue_commit_extent_slow_path(commit_extent const& ce)
{
    do {
        g_shared_input_queue_full_event.signal();
        g_shared_input_consumed_event.wait();
    } while(not g_shared_input_queue.push(ce));
}
void dlog::commit()
{
    using namespace detail;
    auto pib = get_input_buffer();
    pib->commit();
}

