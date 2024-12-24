// This test is inspired by code in ICU.

#include <stdfil.h>
#include <string>

using namespace std;

union Element {
    void* pointer;
    unsigned integer;
};

__attribute__((__noinline__)) bool compareString(const Element key1, const Element key2)
{
    const string* str1 = (const string*)key1.pointer;
    const string* str2 = (const string*)key2.pointer;
    if (str1 == str2)
        return true;
    if (str1 == nullptr || str2 == nullptr)
        return false;
    return *str1 == *str2;
}

__attribute__((__noinline__)) bool compareElements(Element key, Element* element,
                                                   bool (*compare)(const Element, const Element),
                                                   bool isInteger)
{
    if (compare)
        return compare(key, *element);
    if (isInteger)
        return key.integer == element->integer;
    return key.pointer == element->pointer;
}

__attribute__((__noinline__)) bool doCompare(Element key1, Element key2)
{
    return compareElements(key1, &key2, compareString, false);
}

int main()
{
    Element key1;
    Element key2;

    key1.pointer = new string("hello");
    key2.pointer = new string("world");

    ZASSERT(!doCompare(key1, key2));

    key1.pointer = new string("hello");
    key2.pointer = new string("hello");

    ZASSERT(doCompare(key1, key2));

    return 0;
}

