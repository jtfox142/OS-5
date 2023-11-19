#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<time.h>

#define PERMS 0644
#define NUMBER_OF_RESOURCES 10
#define REQUEST_CODE 10
#define TERMINATION_CODE 21
#define MILLISECOND_TIMER 250000000

typedef struct msgbuffer {
	long mtype;
	int intData;
	pid_t childPid;
} msgbuffer;

struct resourceTracker {
	int requests[NUMBER_OF_RESOURCES];
	int allocations[NUMBER_OF_RESOURCES];
};

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
	if(choice <= 65)
		return RNG(9, 0);
	return RNG(19, 10);
}

//Returns 1 if process should terminate
int checkForTermination(struct resourceTracker *resourceTracker) {
	//If there are more requests for a resource than allocations, do not terminate
	for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
		if(resourceTracker->requests[count] > resourceTracker->allocations[count]) {
			return 0;
		}
	}

	return 1;
}

//returns 0 if another request will surpass the boundaries of resource instances 
int addRequest(struct resourceTracker *resourceTracker, int resourceNumber) {
	if(resourceTracker->requests[resourceNumber] >= 20)
		return 0;
	resourceTracker->requests[resourceNumber] = resourceTracker->requests[resourceNumber] + 1;
	return 1;
}

void addAllocation(struct resourceTracker *resourceTracker, int allocationNumber) {
	int resourceNumber = allocationNumber - REQUEST_CODE;
	resourceTracker->allocations[resourceNumber] = resourceTracker->allocations[resourceNumber] + 1;
}

void removeRequest(struct resourceTracker *resourceTracker, int resourceNumber) {
	resourceTracker->requests[resourceNumber] = resourceTracker->requests[resourceNumber] - 1;
}

void removeAllocation(struct resourceTracker *resourceTracker, int allocationNumber) {
	int resourceNumber = allocationNumber - REQUEST_CODE;
	resourceTracker->allocations[resourceNumber] = resourceTracker->allocations[resourceNumber] - 1;
}

void initializeResourceTracker(struct resourceTracker *resourceTracker) {
	for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
		resourceTracker->allocations[count] = 0;
		resourceTracker->requests[count] = 0;
	}
}

int main(int argc, char** argv) {
	//get access to shared memory
	const int sh_key = ftok("./oss.c", 0);
	int shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
	int *simulatedClock = shmat(shm_id, 0, 0);

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

	struct resourceTracker *resourceTracker;
	resourceTracker = malloc(sizeof(struct resourceTracker));
	initializeResourceTracker(resourceTracker);

	/*
	
	MESSAGE LEGEND

	Sending
		Each message will be comprised of an integer, which can be thought of as two bits.
		The first bit will represent the message type.

		0 = request
		1 = release
		2 = terminate

		The second bit will represent the resource that the process is referring to.

		For example, a message of "16" would indicate that the process is wanting to
		release an instance of resource number 6.

		A message of "9" would indicate that the process is requesting an instance of
		resource number 9.

		Any message that has a value of 20 or more will be taken as a notice of termination.

	Receiving
		If a 1 is received, then the requested resource has been granted and the process continues.
		If a 2 is received, then the resource has been released successfully and the process continues.
		If a 0 is received, then the requested resource has not been granted and the process
		goes to sleep until it can receive that resource, at which point it is sent -1, or it is killed.

	*/

	int timer;
	timer = simulatedClock[1];
	int terminate;
	terminate = 0;
	while(1) {
		//If worker has run for at least a second,
		//it will check every 250 ms to see if it should 
		//terminate. If it has been alloted all requested resources, 
		//it will terminate naturally. It might also be asked by
		//OSS to terminate if it is deadlocked.
		if(simulatedClock[0] > 0 && simulatedClock[1] - timer >= MILLISECOND_TIMER) {
			terminate = checkForTermination(resourceTracker);
			timer = simulatedClock[1];

			if(terminate) {
				printf("WORKER %d: Attempting to terminate.", myPid);
				buf.intData = TERMINATION_CODE;
				if(msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
					printf("msgsnd to parent failed.\n");
					exit(1);
				}
				//detach from shared memory
				shmdt(simulatedClock);
				return EXIT_SUCCESS;
			}
		}

		//Send message back to parent
		buf.mtype = parentPid;
		buf.childPid = myPid;
		
		buf.intData = decideAction(); //Returns a number between 0 and 19, inclusive. More likely to return 0-9

		//If the worker is going to request a resource,
		//make sure that we haven't already requested too many of that instance.
		//If we have, then reroll for another resource.
		if(buf.intData < REQUEST_CODE) {
			while(!addRequest(resourceTracker, buf.intData)) {
				buf.intData = RNG(9, 0);
			}
		}

		//Tell parent what we want to do
		if(msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
				printf("msgsnd to parent failed.\n");
			exit(1);
		}

		//Get message back from parent
		msgbuffer rcvbuf;
		if(msgrcv(msqid, &rcvbuf, sizeof(msgbuffer), myPid, 0) == -1) {
			printf("msgrcv failure in child %d\n", myPid);
			exit(1);
		}	

		//If our request was granted, turn the request into an allocation
		if(1 == rcvbuf.intData) {
			removeRequest(resourceTracker, buf.intData);
			addAllocation(resourceTracker, buf.intData);
		} //If the release was granted, remove the allocation
		else if(2 == rcvbuf.intData) {
			removeAllocation(resourceTracker, buf.intData);
		} //If our request was denied go to "sleep" waiting on a message from parent
		else {
			do {
				if(msgrcv(msqid, &rcvbuf, sizeof(msgbuffer), myPid, 0) == -1) {
					printf("msgrcv failure in child %d\n", myPid);
					exit(1);
				}
			}while(rcvbuf.intData != -1);
		}
	}

	//detach from shared memory
	shmdt(simulatedClock);
	return EXIT_SUCCESS;
}
