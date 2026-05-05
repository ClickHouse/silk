#include <silk/util/list.h>

#include <gtest/gtest.h>

namespace silk
{

struct Node
{
    int value;
    ListEntry listEntry;

    explicit Node(int v)
        : value(v)
    {
    }
};

using TestList = List<Node, &Node::listEntry>;

TEST(List, emptyInitially)
{
    TestList list;
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.front(), nullptr);
    EXPECT_EQ(list.back(), nullptr);
}

TEST(List, pushBackOrder)
{
    Node a{1}, b{2}, c{3};
    TestList list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    EXPECT_FALSE(list.empty());
    EXPECT_EQ(list.front(), &a);
    EXPECT_EQ(list.back(), &c);
}

TEST(List, pushFrontOrder)
{
    Node a{1}, b{2}, c{3};
    TestList list;
    list.push_front(&a);
    list.push_front(&b);
    list.push_front(&c);
    EXPECT_EQ(list.front(), &c);
    EXPECT_EQ(list.back(), &a);
}

TEST(List, popFrontReturnsElement)
{
    Node a{1}, b{2};
    TestList list;
    list.push_back(&a);
    list.push_back(&b);
    EXPECT_EQ(list.pop_front(), &a);
    EXPECT_EQ(list.pop_front(), &b);
    EXPECT_EQ(list.pop_front(), nullptr); // empty
    EXPECT_TRUE(list.empty());
}

TEST(List, popBackReturnsElement)
{
    Node a{1}, b{2};
    TestList list;
    list.push_back(&a);
    list.push_back(&b);
    EXPECT_EQ(list.pop_back(), &b);
    EXPECT_EQ(list.pop_back(), &a);
    EXPECT_EQ(list.pop_back(), nullptr); // empty
    EXPECT_TRUE(list.empty());
}

TEST(List, removeLinkedElement)
{
    Node a{1}, b{2}, c{3};
    TestList list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    list.remove(&b);
    EXPECT_EQ(list.front(), &a);
    EXPECT_EQ(list.back(), &c);
    EXPECT_EQ(list.next(&a), &c);
}

TEST(List, removeUnlinkedIsNoOp)
{
    Node a{1}, b{2};
    TestList list;
    list.push_back(&a);
    list.remove(&b); // b was never inserted; must not crash
    EXPECT_EQ(list.front(), &a);
    EXPECT_EQ(list.back(), &a);
}

TEST(List, nextAndPrev)
{
    Node a{1}, b{2}, c{3};
    TestList list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    EXPECT_EQ(list.next(&a), &b);
    EXPECT_EQ(list.next(&b), &c);
    EXPECT_EQ(list.next(&c), nullptr); // last element

    EXPECT_EQ(list.prev(&a), nullptr); // first element
    EXPECT_EQ(list.prev(&b), &a);
    EXPECT_EQ(list.prev(&c), &b);
}

TEST(List, singleElement)
{
    Node a{42};
    TestList list;
    list.push_back(&a);
    EXPECT_EQ(list.front(), &a);
    EXPECT_EQ(list.back(), &a);
    EXPECT_EQ(list.next(&a), nullptr);
    EXPECT_EQ(list.prev(&a), nullptr);
    list.remove(&a);
    EXPECT_TRUE(list.empty());
}

} // namespace silk
