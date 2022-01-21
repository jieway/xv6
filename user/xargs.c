#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


int
main(int argc, char *argv[]) {
    if(argc < 2) {
        printf("Usage: xargs [command]\n");
        exit(-1);
    }
    // 拿到当前进程的标准化输入
    char buf[16];
    read(0, buf, 16);

    // 构造完整参数
    char *xargv[16];
    int xargc = 0;
    for (int i = 1; i < argc; i++) {
        xargv[xargc++] = argv[i];
    }

    // 遍历标准输入
    char *p = buf;
    for (int i = 0; i < 16; i++) {
        if (buf[i] == '\n') {
            if (fork() > 0) {
                p = &buf[i + 1];
                wait(0);
            }else {
                // 把标准输入放入指定位置
                buf[i] = 0;
                xargv[xargc] = p; 
                xargc++;
                xargv[xargc] = 0;
                xargc++;

                exec(xargv[0] , xargv);
                exit(0);
            }
        }
    }
    wait(0);
    exit(0);
}