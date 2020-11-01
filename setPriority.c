#include "types.h"
#include "fcntl.h"
#include "stat.h"
#include "user.h"

int main(int argc, char* argv[])
{
	if(argc<3 || argc>3){
		printf(2,"Usage: setPriority <new_priority> <pid>\n");
		exit();
	}
	int new_priority = atoi(argv[1]);
	int pid = atoi(argv[2]);
	int oldpriority=set_priority(new_priority, pid);
	if(oldpriority==-1)
		printf(2,"setPriority error,\n Pls check priority and pid values\n");
	else printf(1,"Priority set successfully\n");
	exit();
}