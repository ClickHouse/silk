#include <silk/util/tree.h>

#include <gtest/gtest.h>

namespace silk
{

struct Node
{
    uint64_t key;
    TreeEntry treeEntry;

    explicit Node(uint64_t key_)
        : key(key_)
    {
    }

    struct Compare
    {
        bool operator()(const Node & l, const Node & r) const noexcept { return l.key < r.key; }
    };
};

using UniqueTree = Tree<Node, &Node::treeEntry, Node::Compare, false>;
using MultiTree = Tree<Node, &Node::treeEntry, Node::Compare, true>;

TEST(Tree, EmptyTree)
{
    UniqueTree tree;
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.min(), nullptr);
}

TEST(Tree, MinSingleElement)
{
    Node n{42};
    UniqueTree tree;
    tree.insert(&n);
    EXPECT_FALSE(tree.empty());
    EXPECT_EQ(tree.min(), &n);
}

TEST(Tree, MinIsSmallest)
{
    Node a{10}, b{3}, c{7};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.insert(&c);
    EXPECT_EQ(tree.min(), &b);
}

TEST(Tree, InsertReturnsNullOnSuccess)
{
    Node a{1}, b{2};
    UniqueTree tree;
    EXPECT_EQ(tree.insert(&a), nullptr);
    EXPECT_EQ(tree.insert(&b), nullptr);
}

TEST(Tree, InsertReturnsExistingOnDuplicate)
{
    Node a{5}, b{5};
    UniqueTree tree;
    EXPECT_EQ(tree.insert(&a), nullptr);
    EXPECT_EQ(tree.insert(&b), &a);
}

TEST(Tree, DuplicateNotInserted)
{
    Node a{5}, b{5};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.remove(&a);
    EXPECT_TRUE(tree.empty());
}

TEST(Tree, RemoveOnlyElement)
{
    Node n{1};
    UniqueTree tree;
    tree.insert(&n);
    EXPECT_EQ(tree.remove(&n), nullptr);
    EXPECT_TRUE(tree.empty());
}

TEST(Tree, RemoveReturnsSuccessor)
{
    Node a{1}, b{2}, c{3};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.insert(&c);
    EXPECT_EQ(tree.remove(&a), &b);
    EXPECT_EQ(tree.remove(&b), &c);
    EXPECT_EQ(tree.remove(&c), nullptr);
}

TEST(Tree, RemoveMin)
{
    Node a{1}, b{2}, c{3};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.insert(&c);

    tree.remove(tree.min());
    EXPECT_EQ(tree.min(), &b);
    tree.remove(tree.min());
    EXPECT_EQ(tree.min(), &c);
    tree.remove(tree.min());
    EXPECT_TRUE(tree.empty());
}

TEST(Tree, Next)
{
    Node a{1}, b{2}, c{3};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.insert(&c);
    EXPECT_EQ(tree.next(&a), &b);
    EXPECT_EQ(tree.next(&b), &c);
    EXPECT_EQ(tree.next(&c), nullptr);
}

TEST(Tree, Prev)
{
    Node a{1}, b{2}, c{3};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.insert(&c);
    EXPECT_EQ(tree.prev(&a), nullptr);
    EXPECT_EQ(tree.prev(&b), &a);
    EXPECT_EQ(tree.prev(&c), &b);
}

TEST(Tree, FindExisting)
{
    Node a{1}, b{2}, c{3};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    tree.insert(&c);
    EXPECT_EQ(tree.find(&a), &a);
    EXPECT_EQ(tree.find(&b), &b);
    EXPECT_EQ(tree.find(&c), &c);
}

TEST(Tree, FindNonexistent)
{
    Node a{1}, b{3};
    Node key{2};
    UniqueTree tree;
    tree.insert(&a);
    tree.insert(&b);
    EXPECT_EQ(tree.find(&key), nullptr);
}

TEST(Tree, MultisetAllowsDuplicates)
{
    Node a{5}, b{5}, c{5};
    MultiTree tree;
    EXPECT_EQ(tree.insert(&a), nullptr);
    EXPECT_EQ(tree.insert(&b), nullptr);
    EXPECT_EQ(tree.insert(&c), nullptr);

    int count = 0;
    while (!tree.empty())
    {
        EXPECT_EQ(tree.min()->key, 5u);
        tree.remove(tree.min());
        ++count;
    }
    EXPECT_EQ(count, 3);
}

} // namespace silk
