#include <silk/util/error.h>

#include <silk/util/platform.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace silk
{

// std::vector<uint8_t>::data comes from ::operator new, which on hosted implementations is
// aligned to __STDCPP_DEFAULT_NEW_ALIGNMENT__. We need that to be at least alignof(Frame) so that
// Frames placed at offset 0 are aligned; record-padding inside reserve keeps subsequent
// Frames aligned as well.
static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= alignof(Error::Frame), "arena base must satisfy Frame alignment");

char * Error::reserve(int code, const char * file, int line, size_t msgLen) noexcept
{
    // Record size is the Frame header + message + 1 byte reserved for vsnprintf's NUL terminator
    // (unused on plain-message paths). Pad to 8 bytes so consecutive Frames stay 8-aligned.
    size_t recordSize = sizeof(Frame) + msgLen + 1;
    size_t alignedRecordSize = alignUp<size_t>(recordSize, 8);

    size_t newOffset = arena.size();
    if (newOffset >= SENTINEL)
    {
        return nullptr;
    }

    try
    {
        arena.resize(newOffset + alignedRecordSize);
    }
    catch (...)
    {
        return nullptr;
    }

    Frame * frame = std::construct_at(
        reinterpret_cast<Frame *>(arena.data() + newOffset), Frame{topOffset, static_cast<uint32_t>(msgLen), file, code, line});

    topOffset = static_cast<uint32_t>(newOffset);
    return reinterpret_cast<char *>(frame + 1);
}

int Error::push(int code, const char * file, int line, std::string_view message) noexcept
{
    size_t msgLen = std::min(message.size(), MAX_MESSAGE_LEN);

    char * dest = reserve(code, file, line, msgLen);
    if (dest)
    {
        std::memcpy(dest, message.data(), msgLen);
        dest[msgLen] = '\0';
    }
    return code;
}

int Error::pushf(int code, const char * file, int line, const char * format, ...) noexcept
{
    char buffer[MAX_MESSAGE_LEN + 1];

    std::va_list args;
    va_start(args, format);
    int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (written < 0)
    {
        written = 0;
    }

    return push(code, file, line, std::string_view(buffer, static_cast<size_t>(written)));
}

std::string Error::format() const noexcept
{
    std::string result;
    try
    {
        bool first = true;
        for (const Frame * frame = top(); frame; frame = next(frame))
        {
            if (!first)
            {
                result += "\n  caused by: ";
            }
            first = false;

            if (frame->file)
            {
                result += frame->file;
                result += ':';
                result += std::to_string(frame->line);
                result += ": ";
            }

            std::string_view message = frame->message();
            result.append(message.data(), message.size());

            result += " (errno=";
            result += std::to_string(frame->code);
            const char * description = strerrordesc_np(frame->code);
            if (description)
            {
                result += ": ";
                result += description;
            }
            result += ')';
        }
    }
    catch (...)
    {
    }
    return result;
}

} // namespace silk
