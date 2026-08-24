#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace silk
{

/**
 * Read access to one section of a pipeline config file: key = value pairs plus named
 * child sections, in the brace-nested text format:
 *
 *   name {
 *       key = value
 *       child {
 *           ...
 *       }
 *   }
 *
 * One statement per line; # starts a comment. The root reader owns the whole tree;
 * get returns borrowed child readers. Every read marks its key or section consumed
 * so verifyConsumed can flag typos, and a missing or malformed mandatory key aborts
 * with the key's config path.
 */
class ConfigReader
{
public:
    /** Create an empty root reader; read loads it. */
    ConfigReader() = default;

    /**
     * Parse a pipeline config file into this reader; aborts on a syntax error. The
     * file's params section declares substitutable parameters with their defaults;
     * paramOverrides (name = value pairs) replace the defaults, and every $name in a
     * value is then substituted. A $name without a declaration, an override without a
     * declaration, and a declared parameter no value references all abort.
     */
    void read(const char * fileName, const std::vector<std::pair<std::string, std::string>> & paramOverrides = {});

    /** Append the child section names in file order. */
    void list(std::vector<std::string> * names);

    /** Return the child section reader, or null when absent. The parent keeps ownership. */
    ConfigReader * get(const char * name);

    /** Return the key's dotted path within the file - used in error messages. */
    std::string keyPath(const char * name) const;

    /** Abort naming the first key or section nothing ever read - a config typo. */
    void verifyConsumed();

    /** Return the key's raw value; aborts when absent. */
    std::string readString(const char * name);

    /** Return the key's raw value, or nullopt when absent. */
    std::optional<std::string> readStringOpt(const char * name);

    /** readUint64Opt that aborts when the key is absent. */
    uint64_t readUint64(const char * name);

    /** Return the key parsed as a decimal unsigned integer, or nullopt when absent. */
    std::optional<uint64_t> readUint64Opt(const char * name);

    /** readDurationNsOpt that aborts when the key is absent. */
    uint64_t readDurationNs(const char * name);

    /** Return the key parsed as a duration (ns / us / ms / s / m suffix, bare seconds) in nanoseconds, or nullopt when absent. */
    std::optional<uint64_t> readDurationNsOpt(const char * name);

    /** Return the key parsed as a double; aborts when absent. */
    double readDouble(const char * name);

private:
    /** Create the reader for one section at the given dotted path; "" is the file root. */
    explicit ConfigReader(std::string path)
        : path(std::move(path))
    {
    }

    /** One key = value pair and whether any read consumed it. */
    struct Pair
    {
        /** Key name. */
        std::string key;

        /** Raw value text. */
        std::string value;

        /** Set by the first readStringOpt of the key. */
        bool consumed = false;
    };

    /** One child section and whether any get descended into it. */
    struct Child
    {
        /** Section name. */
        std::string name;

        /** Section reader. */
        std::unique_ptr<ConfigReader> reader;

        /** Set by the first get of the section. */
        bool consumed = false;
    };

    /** One substitutable parameter of the params section. */
    struct Param
    {
        /** Parameter name - the text after $. */
        std::string name;

        /** Effective value - the declared default or the override. */
        std::string value;

        /** The declaring pair - marked consumed once any value references the parameter. */
        Pair * declaration;

        /** Set by the first substitution of the parameter. */
        bool used = false;
    };

    //
    // Helpers.
    //

    Pair * findPair(const char * name);
    Child * findChild(const char * name);
    static void substituteSection(ConfigReader * section, std::vector<Param> * params);
    static std::string substituteValue(const std::string & value, const std::string & valuePath, std::vector<Param> * params);
    static std::string_view trim(std::string_view text);

    //
    // State.
    //

    /** Dotted path from the file root - used in error messages. */
    std::string path;

    /** Key = value pairs in file order. */
    std::vector<Pair> pairs;

    /** Child sections in file order. */
    std::vector<Child> children;
};

} // namespace silk
