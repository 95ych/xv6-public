#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int main(int argc, char *argv[])
{
    if(argc<2)
        printf(2,"Insufficient no. of arguments\n");

    int pid = fork();
    if (pid == 0)
    {
        exec(argv[1], argv+1);      
        printf(2, "exec %s failed\n", argv[1]);
        exit();
    }
    else
    {
        int waittime, runtime;
        int ppid = waitx(&waittime, &runtime);
        if(ppid== -1)
        {
            printf(2,"No children exist\n");
        }
        else printf(1, "For pid %d\nRun Time = %d   Wait time= %d\n",pid, runtime, waittime);
        exit();
    }
} 