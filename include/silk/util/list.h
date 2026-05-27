#pragma once

#include <boost/intrusive/list.hpp>

namespace silk
{

/**
 * Intrusive doubly-linked list entry. Embed one member per list membership needed.
 */
using ListEntry = boost::intrusive::list_member_hook<>;

/**
 * Intrusive doubly-linked list.
 *
 * T must embed a ListEntry member pointed to by @p nodePtr.
 * All operations are O(1).
 */
template <typename T, ListEntry T::* nodePtr>
class List
{
public:
    /** Return true if the list contains no objects. */
    bool empty() const noexcept { return impl.empty(); }

    /** Return the first object in the list, or nullptr if the list is empty. */
    T * front() noexcept { return !impl.empty() ? &impl.front() : nullptr; }

    /** Return the last object in the list, or nullptr if the list is empty. */
    T * back() noexcept { return !impl.empty() ? &impl.back() : nullptr; }

    /** Insert @p object at the back of the list. */
    void push_back(T * object) noexcept { impl.push_back(*object); }

    /** Insert @p object at the front of the list. */
    void push_front(T * object) noexcept { impl.push_front(*object); }

    /** Remove and return the first object, or nullptr if the list is empty. */
    T * pop_front() noexcept
    {
        if (!impl.empty())
        {
            T * object = &impl.front();
            impl.pop_front();
            return object;
        }
        return nullptr;
    }

    /** Remove and return the last object, or nullptr if the list is empty. */
    T * pop_back() noexcept
    {
        if (!impl.empty())
        {
            T * object = &impl.back();
            impl.pop_back();
            return object;
        }
        return nullptr;
    }

    /** Remove @p object from the list. No-op if the object is not linked. */
    void remove(T * object) noexcept
    {
        if ((object->*nodePtr).is_linked())
        {
            impl.erase(impl.iterator_to(*object));
        }
    }

    /** Move all elements from @p other to the end of this list. O(1). @p other is left empty. */
    void splice(List * other) noexcept { impl.splice(impl.end(), other->impl); }

    /** Return the next object after @p object, or nullptr if @p object is last. */
    T * next(T * object) noexcept
    {
        auto it = impl.iterator_to(*object);
        ++it;
        return it != impl.end() ? &*it : nullptr;
    }

    /** Return the previous object before @p object, or nullptr if @p object is first. */
    T * prev(T * object) noexcept
    {
        auto it = impl.iterator_to(*object);
        if (it != impl.begin())
        {
            --it;
            return &*it;
        }
        return nullptr;
    }

private:
    using Hook = boost::intrusive::member_hook<T, ListEntry, nodePtr>;
    using Impl = boost::intrusive::list<T, Hook, boost::intrusive::constant_time_size<false>>;

    Impl impl;
};

} // namespace silk
