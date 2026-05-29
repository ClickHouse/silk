#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace silk
{

/**
 * Rich error description that travels alongside an errno-style code returned by a function.
 * An Error is a stack of frames: the top frame is the most recent, the bottom is the innermost
 * failure that started the chain. All frame storage (headers + message bytes) lives in a single
 * arena owned by the Error.
 *
 * Errors are normally produced through the SILK_RETURN_ERROR / SILK_CHECK_ERROR / SILK_CHECK_BOOL macros,
 * which push a frame and capture the call-site file and line. Functions take an Error * error
 * out-parameter and the caller declares the Error on its own stack:
 *
 *     Error error;
 *     int r = doWork(&error);
 *     if (r) {
 *         logger->log(error.format());
 *     }
 *
 * Callers must hand in a clean Error. The macros only push: any pre-existing frames remain
 * underneath the new top frame. Use clear between independent uses of the same Error
 * (e.g., a loop dispatching independent operations).
 *
 * To walk the stack, follow the chain from top via next:
 *
 *     for (const Error::Frame * frame = error.top(); frame; frame = error.next(frame)) {
 *         use(frame->code, frame->file, frame->line, frame->message());
 *     }
 *
 * Frame pointers stay valid until the next push/clear/move/destroy on the owning Error.
 */
class Error
{
public:
    /**
     * One frame stored in the arena. The message bytes follow inline immediately after the header
     * and are always NUL-terminated (the byte one past the last message byte is '\0'), so callers
     * may pass message().data() to C APIs expecting a NUL-terminated string.
     */
    struct Frame
    {
        uint32_t nextOffset; // arena offset of the next-deeper frame, or SENTINEL at the bottom
        uint32_t msgLen;
        const char * file;
        int code;
        int line;

        /** Inline message bytes following this header (NUL-terminated; the NUL is not counted). */
        std::string_view message() const noexcept { return std::string_view(reinterpret_cast<const char *>(this + 1), msgLen); }
    };
    static_assert(sizeof(Frame) == 24);

    /** Maximum length of a single frame's message in bytes; longer messages are truncated. */
    static constexpr size_t MAX_MESSAGE_LEN = 512;

    /** Construct an empty Error with no frames; no heap allocation until the first push. */
    Error() noexcept = default;

    // non-copyable
    Error(const Error &) = delete;
    Error & operator=(const Error &) = delete;

    // moveable; source is left empty (no frames, freshly default-constructed state)
    Error(Error && other) noexcept
        : arena(std::move(other.arena))
        , topOffset(std::exchange(other.topOffset, SENTINEL))
    {
    }
    Error & operator=(Error && other) noexcept
    {
        arena = std::move(other.arena);
        topOffset = std::exchange(other.topOffset, SENTINEL);
        return *this;
    }

    /** True if no frames have been pushed yet. */
    bool empty() const noexcept { return topOffset == SENTINEL; }

    /** errno-style code of the top frame; 0 if empty. */
    int code() const noexcept { return topOffset == SENTINEL ? 0 : frameAt(topOffset)->code; }

    /** Pointer to the top of the stack (most recent frame); nullptr if empty. */
    const Frame * top() const noexcept { return topOffset == SENTINEL ? nullptr : frameAt(topOffset); }

    /** Pointer to the next-deeper frame; nullptr if frame is the innermost. */
    const Frame * next(const Frame * frame) const noexcept { return frame->nextOffset == SENTINEL ? nullptr : frameAt(frame->nextOffset); }

    /** Push one frame on top of the existing stack with a plain message. Returns code. */
    int push(int code, const char * file, int line, std::string_view message) noexcept;

    /** printf-style variant of push: vsnprintf's format + varargs into the frame's message. */
    int pushf(int code, const char * file, int line, const char * format, ...) noexcept __attribute__((format(printf, 5, 6)));

    /**
     * Render the stack as one human-readable string. The top frame prints first; deeper frames are
     * prefixed with "caused by: ". Each frame is <file>:<line>: <message> (errno=<code>[:
     * <description>]). Best-effort: returns whatever was built so far if allocation fails partway.
     */
    std::string format() const noexcept;

    /** Discard all frames. */
    void clear() noexcept
    {
        arena.clear();
        topOffset = SENTINEL;
    }

private:
    /** Marker stored in topOffset / Frame::nextOffset to indicate "no frame". */
    static constexpr uint32_t SENTINEL = UINT32_MAX;

    /** Reserve space for a new top frame; returns a pointer to the message slot, or nullptr on OOM. */
    char * reserve(int code, const char * file, int line, size_t msgLen) noexcept;

    /** Interpret arena bytes at offset as a Frame. */
    const Frame * frameAt(uint32_t offset) const noexcept { return reinterpret_cast<const Frame *>(arena.data() + offset); }

    //
    // State.
    //

    std::vector<uint8_t> arena;
    uint32_t topOffset = SENTINEL;
};

} // namespace silk

/**
 * Unconditional return-and-push: push a fresh frame on top of *error and return code. Captures
 * the call-site file and line. Use when an error condition is detected directly (not propagated
 * from a callee returning a code):
 *
 *     if (input == nullptr) {
 *         SILK_RETURN_ERROR(EINVAL, error, "null input: idx=%d", idx);
 *     }
 */
#define SILK_RETURN_ERROR(code, error, msg, ...) return (error)->push##__VA_OPT__(f)((code), __FILE__, __LINE__, msg __VA_OPT__(, ) __VA_ARGS__)

/**
 * Failure propagation: if r is non-zero, push a new frame on top of *error and return r.
 * Works uniformly for callees that populate *error (the existing stack is preserved underneath)
 * and for callees that do not (the frame becomes the leaf). Callers must hand in a clean Error;
 * see the class doc.
 *
 *     int r = volume->allocateLsn(&lsn);
 *     SILK_CHECK_ERROR(r, error, "could not allocate LSN");
 *
 *     int r = store->getRowBlockReference(rbn, time, rowFunctions, &reference, error);
 *     SILK_CHECK_ERROR(r, error, "could not get row-block reference: pgId=%u", rbn.pgId);
 */
#define SILK_CHECK_ERROR(code, error, msg, ...) \
    do \
    { \
        if ((code) != 0) [[unlikely]] \
        { \
            return (error)->push##__VA_OPT__(f)((code), __FILE__, __LINE__, msg __VA_OPT__(, ) __VA_ARGS__); \
        } \
    } while (0)

/**
 * Boolean failure: if cond is false, push a fresh frame on top of *error and return code.
 *
 * NEVER embed a function call as cond -- capture into a bool b temp first:
 *     bool b = record.addRow(&context, recordRow);
 *     SILK_CHECK_BOOL(b, ENOSPC, error, "could not append row");
 */
#define SILK_CHECK_BOOL(cond, code, error, msg, ...) \
    do \
    { \
        if (!(cond)) [[unlikely]] \
        { \
            return (error)->push##__VA_OPT__(f)((code), __FILE__, __LINE__, msg __VA_OPT__(, ) __VA_ARGS__); \
        } \
    } while (0)
