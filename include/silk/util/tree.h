#pragma once

#include <boost/intrusive/set.hpp>

#include <type_traits>

namespace silk
{

/**
 * Intrusive red-black tree entry. Embed one member per tree membership needed.
 */
using TreeEntry = boost::intrusive::set_member_hook<>;

/**
 * Intrusive red-black tree.
 *
 * T must embed a TreeEntry member pointed to by @p nodePtr.
 * @p Compare is an STL-style less-than comparator functor.
 * @p AllowDuplicates controls whether duplicate keys are allowed.
 * All operations are O(log n).
 */
template <typename T, TreeEntry T::* nodePtr, typename Compare, bool AllowDuplicates = false>
class Tree
{
    using Hook = boost::intrusive::member_hook<T, TreeEntry, nodePtr>;
    using Cmp = boost::intrusive::compare<Compare>;
    using Impl = std::conditional_t<AllowDuplicates, boost::intrusive::multiset<T, Hook, Cmp>, boost::intrusive::set<T, Hook, Cmp>>;

public:
    /** The place findForInsert found for a key it did not find; valid until the tree changes. */
    using InsertPosition = typename boost::intrusive::set<T, Hook, Cmp>::insert_commit_data;

    /**
     * Insert @p object into the tree.
     * Returns nullptr on success. For unique trees, if an equal key already
     * exists the existing element is returned and @p object is not inserted.
     */
    T * insert(T * object) noexcept
    {
        if constexpr (AllowDuplicates)
        {
            impl.insert(*object);
            return nullptr;
        }
        else
        {
            auto [it, inserted] = impl.insert(*object);
            return inserted ? nullptr : &*it;
        }
    }

    /**
     * Remove @p object from the tree. The object must be in this tree.
     * Returns the in-order successor, or nullptr if none.
     */
    T * remove(T * object) noexcept
    {
        auto it = impl.erase(impl.iterator_to(*object));
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return the in-order successor of @p object, or nullptr if it is the maximum.
     */
    T * next(T * object) noexcept
    {
        auto it = impl.iterator_to(*object);
        ++it;
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return the in-order predecessor of @p object, or nullptr if it is the minimum.
     */
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

    /**
     * Return an object matching @p key, or nullptr if none exists.
     */
    T * find(const T * key) noexcept
    {
        auto it = impl.find(*key);
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return an object matching @p key under @p compare, or nullptr if none exists. @p compare orders a key and an object
     * both ways, consistent with the tree's Compare, so no object needs building to search.
     */
    template <typename Key, typename KeyCompare>
    T * find(const Key & key, KeyCompare compare) noexcept
    {
        auto it = impl.find(key, compare);
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return the object matching @p key under @p compare, or nullptr with *@p position set where an insertAt of an
     * object with that key goes. One descent serves the search and the insert; a unique tree only.
     */
    template <typename Key, typename KeyCompare>
    T * findForInsert(const Key & key, KeyCompare compare, InsertPosition * position) noexcept
    {
        static_assert(!AllowDuplicates, "findForInsert needs a tree of unique keys");
        auto [it, missing] = impl.insert_check(key, compare, *position);
        return missing ? nullptr : &*it;
    }

    /** Insert @p object, whose key findForInsert did not find, at @p position; the tree must not change in between. */
    void insertAt(T * object, const InsertPosition & position) noexcept
    {
        static_assert(!AllowDuplicates, "insertAt needs a tree of unique keys");
        impl.insert_commit(*object, position);
    }

    /**
     * Return the object with the smallest key not less than @p key's, or nullptr if none.
     */
    T * lowerBound(const T * key) noexcept
    {
        auto it = impl.lower_bound(*key);
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return the object with the smallest key strictly greater than @p key's, or nullptr if none.
     */
    T * upperBound(const T * key) noexcept
    {
        auto it = impl.upper_bound(*key);
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return the object with the smallest key, or nullptr if the tree is empty.
     */
    T * min() noexcept
    {
        auto it = impl.begin();
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return the object with the largest key, or nullptr if the tree is empty.
     */
    T * max() noexcept
    {
        auto it = impl.rbegin();
        return it != impl.rend() ? &*it : nullptr;
    }

    /**
     * Return true if the tree contains no objects.
     */
    bool empty() const noexcept { return impl.empty(); }

private:
    Impl impl;
};

} // namespace silk
