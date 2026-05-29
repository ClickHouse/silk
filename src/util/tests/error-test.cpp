#include <silk/util/error.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

namespace silk
{
namespace
{

int returnLeaf(Error * error, int * macroLine)
{
    *macroLine = __LINE__ + 1;
    SILK_RETURN_ERROR(EIO, error, "disk failure");
    return 0;
}

int returnLeafFormatted(Error * error, int * macroLine)
{
    *macroLine = __LINE__ + 1;
    SILK_RETURN_ERROR(EIO, error, "could not open %s: errno=%d", "foo.txt", 42);
    return 0;
}

int checkErrorPropagates(int innerCode, Error * error, int * macroLine)
{
    int r = innerCode;
    *macroLine = __LINE__ + 1;
    SILK_CHECK_ERROR(r, error, "could not allocate LSN");
    return 0;
}

int checkErrorFormatted(int innerCode, Error * error, int * macroLine)
{
    int r = innerCode;
    *macroLine = __LINE__ + 1;
    SILK_CHECK_ERROR(r, error, "could not insert record: pgId=%u", 7u);
    return 0;
}

int checkErrorStacks(Error * error, int * macroLine)
{
    int r = error->push(EIO, "log.cpp", 11, "ring buffer full");
    *macroLine = __LINE__ + 1;
    SILK_CHECK_ERROR(r, error, "log write rejected at lsn=%lu", 42UL);
    return 0;
}

int checkBoolPropagates(bool cond, Error * error, int * macroLine)
{
    *macroLine = __LINE__ + 1;
    SILK_CHECK_BOOL(cond, ENOSPC, error, "could not append row");
    return 0;
}

bool endsWith(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size())
    {
        return false;
    }
    return text.substr(text.size() - suffix.size()) == suffix;
}

}

TEST(ErrorTest, DefaultConstructedIsEmpty)
{
    Error error;
    bool empty = error.empty();
    ASSERT_TRUE(empty);

    int code = error.code();
    ASSERT_EQ(code, 0);

    const Error::Frame * top = error.top();
    ASSERT_EQ(top, nullptr);
}

TEST(ErrorTest, ClearDiscardsFrames)
{
    Error error;
    error.push(EIO, "demo.cpp", 1, "disk failure");

    error.clear();

    bool empty = error.empty();
    ASSERT_TRUE(empty);
}

TEST(ReturnErrorTest, WritesLeafAndReturnsCode)
{
    Error error;
    int macroLine = 0;
    int r = returnLeaf(&error, &macroLine);
    ASSERT_EQ(r, EIO);

    int code = error.code();
    ASSERT_EQ(code, EIO);

    const Error::Frame * top = error.top();
    ASSERT_NE(top, nullptr);

    int topCode = top->code;
    ASSERT_EQ(topCode, EIO);

    std::string_view topMessage = top->message();
    ASSERT_EQ(topMessage, "disk failure");

    int topLine = top->line;
    ASSERT_EQ(topLine, macroLine);

    const char * topFile = top->file;
    ASSERT_TRUE(endsWith(topFile, "error-test.cpp"));

    const Error::Frame * deeper = error.next(top);
    ASSERT_EQ(deeper, nullptr);
}

TEST(ReturnErrorTest, FormatsMessage)
{
    Error error;
    int macroLine = 0;
    int r = returnLeafFormatted(&error, &macroLine);
    ASSERT_EQ(r, EIO);

    const Error::Frame * top = error.top();
    std::string_view message = top->message();
    ASSERT_EQ(message, "could not open foo.txt: errno=42");

    int line = top->line;
    ASSERT_EQ(line, macroLine);
}

TEST(CheckErrorTest, FallsThroughOnSuccess)
{
    Error error;
    int macroLine = 0;
    int r = checkErrorPropagates(0, &error, &macroLine);
    ASSERT_EQ(r, 0);

    bool empty = error.empty();
    ASSERT_TRUE(empty);
}

TEST(CheckErrorTest, PropagatesLeafFailure)
{
    Error error;
    int macroLine = 0;
    int r = checkErrorPropagates(EIO, &error, &macroLine);
    ASSERT_EQ(r, EIO);

    const Error::Frame * top = error.top();
    ASSERT_NE(top, nullptr);

    int code = top->code;
    ASSERT_EQ(code, EIO);

    std::string_view message = top->message();
    ASSERT_EQ(message, "could not allocate LSN");

    int line = top->line;
    ASSERT_EQ(line, macroLine);

    const Error::Frame * deeper = error.next(top);
    ASSERT_EQ(deeper, nullptr);
}

TEST(CheckErrorTest, FormatsLeafFailure)
{
    Error error;
    int macroLine = 0;
    int r = checkErrorFormatted(EBADF, &error, &macroLine);
    ASSERT_EQ(r, EBADF);

    const Error::Frame * top = error.top();
    std::string_view message = top->message();
    ASSERT_EQ(message, "could not insert record: pgId=7");

    int line = top->line;
    ASSERT_EQ(line, macroLine);
}

TEST(CheckErrorTest, StacksOnTopOfExistingFrame)
{
    Error error;
    int macroLine = 0;
    int r = checkErrorStacks(&error, &macroLine);
    ASSERT_EQ(r, EIO);

    const Error::Frame * top = error.top();
    ASSERT_NE(top, nullptr);

    int topCode = top->code;
    ASSERT_EQ(topCode, EIO);

    std::string_view topMessage = top->message();
    ASSERT_EQ(topMessage, "log write rejected at lsn=42");

    int topLine = top->line;
    ASSERT_EQ(topLine, macroLine);

    const Error::Frame * bottom = error.next(top);
    ASSERT_NE(bottom, nullptr);

    int bottomCode = bottom->code;
    ASSERT_EQ(bottomCode, EIO);

    std::string_view bottomMessage = bottom->message();
    ASSERT_EQ(bottomMessage, "ring buffer full");

    const Error::Frame * endOfChain = error.next(bottom);
    ASSERT_EQ(endOfChain, nullptr);
}

TEST(CheckBoolTest, FallsThroughOnTrue)
{
    Error error;
    int macroLine = 0;
    int r = checkBoolPropagates(true, &error, &macroLine);
    ASSERT_EQ(r, 0);

    bool empty = error.empty();
    ASSERT_TRUE(empty);
}

TEST(CheckBoolTest, PropagatesOnFalse)
{
    Error error;
    int macroLine = 0;
    int r = checkBoolPropagates(false, &error, &macroLine);
    ASSERT_EQ(r, ENOSPC);

    const Error::Frame * top = error.top();
    int code = top->code;
    ASSERT_EQ(code, ENOSPC);

    std::string_view message = top->message();
    ASSERT_EQ(message, "could not append row");

    int line = top->line;
    ASSERT_EQ(line, macroLine);
}

TEST(FormatTest, SingleFrameRendersAllFields)
{
    Error error;
    error.push(EIO, "demo.cpp", 42, "disk failure");

    std::string formatted = error.format();
    std::string expected = std::string("demo.cpp:42: disk failure (errno=") + std::to_string(EIO) + ": " + std::strerror(EIO) + ")";
    ASSERT_EQ(formatted, expected);
}

TEST(FormatTest, StackRendersTopFirstWithCausedBy)
{
    Error error;
    error.push(ENOSPC, "log.cpp", 201, "ring buffer full");
    error.push(ENOSPC, "volume.cpp", 89, "could not allocate LSN");
    error.push(EIO, "store.cpp", 142, "could not insert record: pgId=7");

    std::string formatted = error.format();
    std::string expected = std::string("store.cpp:142: could not insert record: pgId=7 (errno=") + std::to_string(EIO) + ": "
        + std::strerror(EIO) + ")\n  caused by: volume.cpp:89: could not allocate LSN (errno=" + std::to_string(ENOSPC) + ": "
        + std::strerror(ENOSPC) + ")\n  caused by: log.cpp:201: ring buffer full (errno=" + std::to_string(ENOSPC) + ": "
        + std::strerror(ENOSPC) + ")";
    ASSERT_EQ(formatted, expected);
}

} // namespace silk
