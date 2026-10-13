#include "rsx_profile_buffer.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); return 1; } } while (0)

int main(void)
{
    const char* path = "profile_buffer_test.csv";
    rsx_profile_buffer b = {0};
    remove(path);
    CHECK(!rsx_profile_buffer_init(&b,path,"header\n",SIZE_MAX,16));
    CHECK(!rsx_profile_buffer_init(&b,path,"header\n",3,8193));
    CHECK(rsx_profile_buffer_init(&b,path,"frame,value\n",3,32));
    CHECK(!rsx_profile_buffer_init(&b,path,"header\n",3,32));
    CHECK(rsx_profile_buffer_printf(&b,"%u,%s\n",1,"one"));
    CHECK(rsx_profile_buffer_printf(&b,"%u,%s\n",2,"two"));
    CHECK(rsx_profile_buffer_printf(&b,"%u,%s\n",3,"three"));
    CHECK(rsx_profile_buffer_printf(&b,"%u,%s\n",4,"four"));
    CHECK(!rsx_profile_buffer_printf(&b,"%040u\n",5));
    CHECK(!rsx_profile_buffer_printf(&b,"no newline"));
    CHECK(b.recorded == 4 && b.rejected == 2);
    CHECK(fopen(path,"rb") == NULL); /* Recording never opens the destination. */
    CHECK(rsx_profile_buffer_dump(&b));
    FILE* f = fopen(path,"rb"); CHECK(f != NULL);
    char data[128] = {0};
    const size_t n = fread(data,1,sizeof(data)-1,f); fclose(f);
    CHECK(n == strlen("frame,value\n2,two\n3,three\n4,four\n"));
    CHECK(strcmp(data,"frame,value\n2,two\n3,three\n4,four\n") == 0);
    CHECK(rsx_profile_buffer_dump(&b)); /* Repeat dump preserves records. */
    rsx_profile_buffer_destroy(&b);
    CHECK(!b.rows && !b.recorded && !rsx_profile_buffer_dump(&b));
    CHECK(rsx_profile_buffer_init(&b,path,"empty\n",1,16));
    CHECK(rsx_profile_buffer_dump(&b));
    f=fopen(path,"rb"); CHECK(f != NULL);
    CHECK(fgets(data,sizeof(data),f) != NULL && strcmp(data,"empty\n") == 0);
    CHECK(fgetc(f) == EOF); fclose(f);
    b.path[0]='\0'; CHECK(!rsx_profile_buffer_dump(&b));
    rsx_profile_buffer_destroy(&b);
    remove(path);
    puts("deferred profile buffer: PASS");
    return 0;
}
