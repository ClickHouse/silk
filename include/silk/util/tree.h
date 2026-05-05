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
public:
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
     * Return the object with the smallest key, or nullptr if the tree is empty.
     */
    T * min() noexcept
    {
        auto it = impl.begin();
        return it != impl.end() ? &*it : nullptr;
    }

    /**
     * Return true if the tree contains no objects.
     */
    bool empty() const noexcept { return impl.empty(); }

private:
    using Hook = boost::intrusive::member_hook<T, TreeEntry, nodePtr>;
    using Cmp = boost::intrusive::compare<Compare>;
    using Impl = std::conditional_t<AllowDuplicates, boost::intrusive::multiset<T, Hook, Cmp>, boost::intrusive::set<T, Hook, Cmp>>;

    Impl impl;
};

} // namespace silk
