#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<time.h>

#define PERMS 0644

typedef struct msgbuffer {
	long mtype;
	int intData;
} msgbuffer;

int RNG(int max, int min) {
	unsigned int randval;
	FILE *f;
   	f = fopen("/dev/urandom", "r");
	fread(&randval, sizeof(randval), 1, f);
	fclose(f);

	srand(randval);
	return ((rand() % (max - min + 1) + 1));
}

int decideAction() {
	int choice = RNG(100, 0);
	if(choice < 95)
		return 1;
	if(choice <= 98)
		return 2;
	return 3;
}

int decideTimeUsed(msgbuffer buf) {
	int timeUsed = RNG(buf.intData, 1);
	return timeUsed;
}

int main(int argc, char** argv) {
	msgbuffer buf;
	buf.mtype = 1;
	buf.intData = 0;
	int msqid = 0;
	key_t key;

	// get a key for our message queue
	if ((key = ftok("msgq.txt", 1)) == -1) {
		perror("ftok");
		exit(1);
	}

	// create our message queue
	if ((msqid = msgget(key, PERMS)) == -1) {
		perror("msgget in child");
		exit(1);
	}	
       	
	pid_t parentPid = getppid();
	pid_t myPid = getpid();

	int terminate = 0;

	while(!terminate) {
		int msgReceived; //set to 1 when message comes in from parent
		msgReceived = 0;
		while(!msgReceived) {
			if(msgrcv(msqid, &buf, sizeof(msgbuffer), myPid, 0) >= 0) {
				msgReceived = 1;
			}
		}

		int action = decideAction();
		if(action == 2) {
			buf.intData = decideTimeUsed(buf);
		}
		else if(action == 3) {
			buf.intData = -decideTimeUsed(buf);
			terminate = 1;
		}

		//Send message back to parent
		buf.mtype = parentPid;
		if(msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
			printf("msgsnd to parent failed.\n");
			exit(1);
		}
	}

	return EXIT_SUCCESS;
}
