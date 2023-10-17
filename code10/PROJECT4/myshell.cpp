#include<iostream>
#include<string>
#include<unistd.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<cstdio>
#include<cstring>
#include<cstdlib>

using namespace std;

#define NUM 32
#define SIZE 256

int main()
{
	//打印提示符
	const string tip = "rikaTang@ ";
	char* argv[NUM] = {NULL};
	while(1)
	{
		//获取命令行字符串
		char cmd[SIZE];
		cout<<tip;
		fgets(cmd,sizeof(cmd)-1,stdin);
		cmd[strlen(cmd)-1] = 0;
		//解析字符串
		int i = 0;
		argv[i++] = strtok(cmd," ");
		while(1)
		{
			argv[i] = strtok(NULL," ");
			if(argv[i] == NULL)
				break;
			i++;
		}
	
		//命令执行
		pid_t id = fork();
		if(id == 0)
		{
			execvp(argv[0],argv);
			exit(13);
		}
		else
		{
			int st = 0;
			waitpid(id,&st,0);
		}
	}
	
	return 0;
}
