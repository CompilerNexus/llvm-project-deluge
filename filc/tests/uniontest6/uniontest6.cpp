// This test is inspired by code in ICU.

#include <stdfil.h>
#include <string>

using namespace std;

union Element {
    void* pointer;
    int integer;
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

__attribute__((__noinline__)) int indexOf(Element key, Element* elements, int count,
                                          bool (*compare)(const Element, const Element),
                                          bool isInteger)
{
    if (compare) {
        for (int i = 0; i < count; ++i) {
            if (compare(key, elements[i]))
                return i;
        }
    } else {
        for (int i = 0; i < count; ++i) {
            if (isInteger) {
                if (key.integer == elements[i].integer)
                    return i;
            } else {
                if (key.pointer == elements[i].pointer)
                    return i;
            }
        }
    }
    return -1;
}

int main()
{
    Element key;
    Element* elements;
    int count = 100;

    key.pointer = new string("foo 42");
    
    elements = new Element[count];
    for (int i = 0; i < count; ++i)
        elements[i].pointer = new string(zasprintf("foo %d", i));

    ZASSERT(indexOf(key, elements, count, compareString, false) == 42);

    return 0;
}

