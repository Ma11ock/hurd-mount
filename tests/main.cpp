#include <gtest/gtest.h>

extern "C"
{
#include "../libshouldbeinlibc/mount.c"
}

/* Cannot use normal string comp because \0 does not terminate the strings */
bool myStringCompare(const char *str1, const char *str2,
                     size_t sizeofStr1,size_t sizeofStr2)
{
    if(sizeofStr1 != sizeofStr2)
        return false;

    for(size_t i = 0; i < sizeofStr1; i++)
    {
        if(str1[i] != str2[i])
            return false;
    }

    return true;
}

TEST(myTest, mtest)
{
    char *teststr = NULL;
    size_t testlen = 0;
    const char *strs = "test1str\0test2str\0test3str"

    mnt_append_option(&teststr, &testlen, "test1str");
    mnt_append_option(&teststr, &testlen, "test2str");
    mnt_append_option(&teststr, &testlen, "test3str");

    ASSERT_EQ(myStringCompare(teststr, strs, testlen, sizeof(strs), true);
    free(teststr);
}