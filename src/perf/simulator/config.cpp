#include "config.h"

#include <perf/util/parse.h>
#include <silk/util/assert.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <utility>

namespace silk
{

void ConfigReader::read(const char * fileName, const std::vector<std::pair<std::string, std::string>> & paramOverrides)
{
    bool fresh = path.empty() && pairs.empty() && children.empty();
    SILK_ASSERT(fresh, "read loads the empty root reader once");

    std::ifstream file(fileName);
    SILK_ASSERT(file.is_open(), "could not open the config file %s", fileName);

    std::vector<ConfigReader *> stack{this};

    std::string line;
    int lineNumber = 0;

    while (std::getline(file, line))
    {
        ++lineNumber;

        size_t comment = line.find('#');

        if (comment != std::string::npos)
        {
            line.resize(comment);
        }

        std::string_view text = trim(line);

        if (text.empty())
        {
            continue;
        }

        if (text == "}")
        {
            stack.pop_back();
            SILK_ASSERT(!stack.empty(), "config %s line %d: unmatched }", fileName, lineNumber);
            continue;
        }

        ConfigReader * section = stack.back();

        if (text.back() == '{')
        {
            text.remove_suffix(1);
            std::string name(trim(text));

            bool valid = !name.empty() && name.find_first_of(" \t={}") == std::string::npos;
            SILK_ASSERT(valid, "config %s line %d: invalid section name '%s'", fileName, lineNumber, name.c_str());

            Child * duplicate = section->findChild(name.c_str());
            SILK_ASSERT(!duplicate, "config %s line %d: duplicate section '%s'", fileName, lineNumber, name.c_str());

            std::string childPath = section->path.empty() ? name : section->path + "." + name;
            std::unique_ptr<ConfigReader> child(new ConfigReader(std::move(childPath)));
            stack.push_back(child.get());
            section->children.push_back({name, std::move(child)});
            continue;
        }

        size_t equals = text.find('=');
        SILK_ASSERT(equals != std::string_view::npos, "config %s line %d: expected 'name {', 'key = value', or '}'", fileName, lineNumber);

        std::string key(trim(text.substr(0, equals)));
        std::string value(trim(text.substr(equals + 1)));

        bool filled = !key.empty() && !value.empty();
        SILK_ASSERT(filled, "config %s line %d: empty key or value", fileName, lineNumber);

        Pair * duplicate = section->findPair(key.c_str());
        SILK_ASSERT(!duplicate, "config %s line %d: duplicate key '%s'", fileName, lineNumber, key.c_str());

        section->pairs.push_back({std::move(key), std::move(value)});
    }

    SILK_ASSERT(stack.size() == 1, "config %s: unterminated section", fileName);

    // Collect the declared params, apply the overrides, and substitute every $name.
    std::vector<Param> params;
    Child * paramsChild = findChild("params");

    if (paramsChild)
    {
        paramsChild->consumed = true;

        bool flat = paramsChild->reader->children.empty();
        SILK_ASSERT(flat, "config %s: the params section takes no child sections", fileName);

        for (Pair & pair : paramsChild->reader->pairs)
        {
            params.push_back({pair.key, pair.value, &pair});
        }
    }

    for (const std::pair<std::string, std::string> & paramOverride : paramOverrides)
    {
        Param * param = nullptr;

        for (Param & candidate : params)
        {
            if (candidate.name == paramOverride.first)
            {
                param = &candidate;
                break;
            }
        }

        SILK_ASSERT(param, "config %s declares no param '%s'", fileName, paramOverride.first.c_str());

        // Write through to the declaring pair - the run settings (duration, warmup,
        // seed) are read from the params section directly, not by substitution.
        param->value = paramOverride.second;
        param->declaration->value = paramOverride.second;
    }

    for (Child & child : children)
    {
        if (&child != paramsChild)
        {
            substituteSection(child.reader.get(), &params);
        }
    }

    // A used param is a consumed declaration; an unused one trips verifyConsumed.
    for (const Param & param : params)
    {
        if (param.used)
        {
            param.declaration->consumed = true;
        }
    }
}

void ConfigReader::list(std::vector<std::string> * names)
{
    for (const Child & child : children)
    {
        names->push_back(child.name);
    }
}

ConfigReader * ConfigReader::get(const char * name)
{
    Child * child = findChild(name);

    if (child)
    {
        child->consumed = true;
        return child->reader.get();
    }

    return nullptr;
}

std::string ConfigReader::keyPath(const char * name) const
{
    if (!path.empty())
    {
        return path + "." + name;
    }

    return name;
}

void ConfigReader::verifyConsumed()
{
    for (const Pair & pair : pairs)
    {
        SILK_ASSERT(pair.consumed, "config key %s was never read - misspelled or misplaced?", keyPath(pair.key.c_str()).c_str());
    }

    for (const Child & child : children)
    {
        SILK_ASSERT(child.consumed, "config section %s was never read - misspelled or misplaced?", keyPath(child.name.c_str()).c_str());
        child.reader->verifyConsumed();
    }
}

std::string ConfigReader::readString(const char * name)
{
    std::optional<std::string> value = readStringOpt(name);

    SILK_ASSERT(value, "config key %s is missing", keyPath(name).c_str());
    return std::move(*value);
}

std::optional<std::string> ConfigReader::readStringOpt(const char * name)
{
    Pair * pair = findPair(name);

    if (pair)
    {
        pair->consumed = true;
        return pair->value;
    }

    return std::nullopt;
}

uint64_t ConfigReader::readUint64(const char * name)
{
    std::optional<uint64_t> value = readUint64Opt(name);

    SILK_ASSERT(value, "config key %s is missing", keyPath(name).c_str());
    return *value;
}

std::optional<uint64_t> ConfigReader::readUint64Opt(const char * name)
{
    std::optional<std::string> value = readStringOpt(name);

    if (!value)
    {
        return std::nullopt;
    }

    // strtoull silently wraps a negative to a huge value - refuse the sign outright.
    char * end = nullptr;
    uint64_t parsed = std::strtoull(value->c_str(), &end, 10);

    bool numeric = !value->empty() && *end == '\0' && value->front() != '-';
    SILK_ASSERT(numeric, "config key %s: '%s' is not an unsigned integer", keyPath(name).c_str(), value->c_str());

    return parsed;
}

uint64_t ConfigReader::readDurationNs(const char * name)
{
    std::optional<uint64_t> value = readDurationNsOpt(name);

    SILK_ASSERT(value, "config key %s is missing", keyPath(name).c_str());
    return *value;
}

std::optional<uint64_t> ConfigReader::readDurationNsOpt(const char * name)
{
    std::optional<std::string> value = readStringOpt(name);

    if (!value)
    {
        return std::nullopt;
    }

    try
    {
        return parseDuration(*value);
    }
    catch (const std::exception &)
    {
        SILK_FAIL("config key %s: '%s' is not a duration", keyPath(name).c_str(), value->c_str());
    }
}

double ConfigReader::readDouble(const char * name)
{
    std::string value = readString(name);
    char * end = nullptr;
    double parsed = std::strtod(value.c_str(), &end);

    bool numeric = !value.empty() && *end == '\0';
    SILK_ASSERT(numeric, "config key %s: '%s' is not a number", keyPath(name).c_str(), value.c_str());

    return parsed;
}

ConfigReader::Pair * ConfigReader::findPair(const char * name)
{
    for (Pair & pair : pairs)
    {
        if (pair.key == name)
        {
            return &pair;
        }
    }

    return nullptr;
}

ConfigReader::Child * ConfigReader::findChild(const char * name)
{
    for (Child & child : children)
    {
        if (child.name == name)
        {
            return &child;
        }
    }

    return nullptr;
}

void ConfigReader::substituteSection(ConfigReader * section, std::vector<Param> * params)
{
    for (Pair & pair : section->pairs)
    {
        if (pair.value.find('$') != std::string::npos)
        {
            pair.value = substituteValue(pair.value, section->keyPath(pair.key.c_str()), params);
        }
    }

    for (Child & child : section->children)
    {
        substituteSection(child.reader.get(), params);
    }
}

std::string ConfigReader::substituteValue(const std::string & value, const std::string & valuePath, std::vector<Param> * params)
{
    std::string result;

    for (size_t i = 0; i < value.size();)
    {
        if (value[i] != '$')
        {
            result += value[i];
            ++i;
            continue;
        }

        size_t nameEnd = i + 1;

        while (nameEnd < value.size() && (std::isalnum(static_cast<unsigned char>(value[nameEnd])) || value[nameEnd] == '_'))
        {
            ++nameEnd;
        }

        std::string name = value.substr(i + 1, nameEnd - i - 1);

        SILK_ASSERT(!name.empty(), "config key %s: stray $ in '%s'", valuePath.c_str(), value.c_str());

        Param * param = nullptr;

        for (Param & candidate : *params)
        {
            if (candidate.name == name)
            {
                param = &candidate;
                break;
            }
        }

        if (!param)
        {
            SILK_FAIL("config key %s references the undeclared param $%s", valuePath.c_str(), name.c_str());
        }

        param->used = true;
        result += param->value;
        i = nameEnd;
    }

    return result;
}

std::string_view ConfigReader::trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }

    return text;
}

} // namespace silk
