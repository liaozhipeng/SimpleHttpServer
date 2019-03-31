#include "server.h"

int main(int argc, char * argv[])
{

	GetParaFromCmd(argc, argv);
	GetParaFromFile(server_para.ConfigFile);
	DisplayConf();
	Gthread_pool pool;
	server_init(&pool);
	while(1);
	server_close(&pool);
	return 0;
}
