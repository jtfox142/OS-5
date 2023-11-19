#include<unistd.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<time.h>
#include<signal.h>
#include<sys/msg.h>
#include<errno.h>

#define PERMS 0644
#define MAX_CHILDREN 18
#define ONE_SECOND 1000000000
#define HALF_SECOND 500000000
#define STANDARD_CLOCK_INCREMENT 10000
#define RESOURCE_TABLE_SIZE 10

typedef struct msgBuffer {
	long mtype;
	int intData;
	pid_t childPid;
} msgBuffer;

struct PCB {
	int occupied; //either true or false
	pid_t pid; //process id of this child
	int startTimeSeconds; //time when it was created
	int startTimeNano; //time when it was created
	int blocked; //is this process waiting on a resource?
	int requestVector[10]; //represents how many instances of each resource have been requested
	int allocationVector[10]; //represents how many instances of each resource have been granted
};

struct resource {
	int totalInstances;
	int availableInstances;
	/*
		TODO: Change requestVector to a requestQueue inside of resource.
		It will be a queue of linked lists that hold both the number of the requested resource
		and the simulated time at which it was requested. Then, I can attempt to grant outstanding
		requests first.
	*/
};

// GLOBAL VARIABLES
//For storing each child's PCB. Memory is allocated in main
struct PCB *processTable;
//For storing resources
static struct resource *resourceTable;
//Message queue id
int msqid;
//Needed for killing all child processes
int processTableSize;
//Needed for launching purposes
int runningChildren;
//Output file
FILE *fptr;
//Shared memory variables
int sh_key;
int shm_id;
int *simulatedClock;
//message buffer for passing messages
msgBuffer buf;

// FUNCTION PROTOTYPES

//Help function
void help();

//Process table functions
void initializeProcessTable();
void startInitialProcesses(int initialChildren);
void initializePCB(pid_t pid);
void processEnded(int pidNumber);
void outputTable();

//OSS functions
void incrementClock(int timePassed);
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime);
void checkForMessages();
void updateTable(pid_t process, msgBuffer rcvbuf);
void nonblockWait();
void startInitialProcesses(int initialChildren);

//Program end functions
void terminateProgram(int signum);
void sighandler(int signum);

//Log file functions
void sendingOutput(int chldNum, int chldPid, int systemClock[2]);
void receivingOutput(int chldNum, int chldPid, int systemClock[2], msgBuffer rcvbuf);

//Resource Functions
void grantResource(pid_t childPid, int resourceNumber, int processNumber);
void initializeResourceTable();
void request(pid_t childPid, int resourceNumber);
int release(pid_t childPid, int resourceNumber);

//Queue functions
int addItemToQueue(pid_t *queue, pid_t itemToAdd);
int removeItemFromQueue(pid_t *queue, pid_t itemToRemove);
void initializeQueue(pid_t *queue);

//Helper functions
int checkChildren(int maxSimulChildren);
int stillChildrenToLaunch();
int childrenInSystem();
int findTableIndex(pid_t pid);
void checkTime(int *outputTimer, int *deadlockDetectionTimer);
void takeAction(pid_t childPid, int msgData);
void childTerminated(pid_t terminatedChild);
void sendMessage(pid_t childPid, int msg);

/* 

TODO
* Start deadlock detection algo (just alert and end)
* Finish deadlock detection algo (decide which processes to terminate)

*/

int main(int argc, char** argv) {
	//signals to terminate program properly if user hits ctrl+c or 60 seconds pass
	alarm(60);
	signal(SIGALRM, sighandler);
	signal(SIGINT, sighandler);	

	//allocate shared memory
	sh_key = ftok("./oss.c", 0);
	shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
	if(shm_id <= 0) {
		printf("Shared memory allocation failed\n");
		exit(1);
	}

	//attach to shared memory
	simulatedClock = shmat(shm_id, 0 ,0);
	if(simulatedClock <= 0) {
		printf("Attaching to shared memory failed\n");
		exit(1);
	}

	//set clock to zero
    simulatedClock[0] = 0;
    simulatedClock[1] = 0;

	runningChildren = 0;
	

	//message queue setup
	key_t key;
	system("touch msgq.txt");

	//get a key for our message queue
	if ((key = ftok("msgq.txt", 1)) == -1) {
		perror("ftok");
		exit(1);
	}

	//create our message queue
	if ((msqid = msgget(key, PERMS | IPC_CREAT)) == -1) {
		perror("msgget in parent");
		exit(1);
	}

	//user input variables
	int option;
	int proc;
	int simul;
	int timelimit;

	while ((option = getopt(argc, argv, "hn:s:t:f:")) != -1) {
  		switch(option) {
   			case 'h':
    				help();
    				break;
   			case 'n':
    				proc = atoi(optarg);
    				break;
   			case 's':
				simul = atoi(optarg);
				break;
			case 't':
				timelimit = atoi(optarg);
				break;
			case'f':
				fptr = fopen(optarg, "a");
		}
	}

	if(proc > MAX_CHILDREN) {
		printf("Warning: The maximum value of proc is 18. Terminating.\n");
		terminateProgram(6);
	}
	
	//sets the global var equal to the user arg
	processTableSize = proc;	

	//allocates memory for the processTable stored in global memory
	processTable = calloc(processTableSize, sizeof(struct PCB));
	//sets all pids in the process table to 0
	initializeProcessTable();

	resourceTable = malloc(RESOURCE_TABLE_SIZE * sizeof(struct resource));
	initializeResourceTable();

	startInitialProcesses(simul);

	int *outputTimer = malloc(sizeof(int));
	*outputTimer = 0;

	int *deadlockDetectionTimer = malloc(sizeof(int));
	*deadlockDetectionTimer = 0;

	int *lastLaunchTime = malloc(sizeof(int));
	*lastLaunchTime = 0;

	//stillChildrenToLaunch checks if we have initialized the final PCB yet. 
	//childrenInSystem checks if any PCBs remain occupied
	while(stillChildrenToLaunch() || childrenInSystem()) {
		
		//Nonblocking waitpid to see if a child has terminated.
		nonblockWait();

		//calls another function to check if runningChildren < simul and if timeLimit has been passed
		//if so, it launches a new child.
		launchChild(simul, timelimit, lastLaunchTime);

		//Try to grant any outstanding requests 
		//checkOutstandingRequests();//TODO

		//checks to see if a message has been sent to parent
		//if so, then it takes proper action
		checkForMessages();

		//outputs the process table to a log file and the screen every half second
		//and runs a deadlock detection algorithm every second
		checkTime(outputTimer, deadlockDetectionTimer);

		incrementClock(STANDARD_CLOCK_INCREMENT);
	}

	pid_t wpid;
	int status = 0;
	while((wpid = wait(&status)) > 0);
	terminateProgram(SIGTERM);
	return EXIT_SUCCESS;
}

// FUNCTION DEFINITIONS

void startInitialProcesses(int initialChildren) {
	int lowerValue;
	(initialChildren < processTableSize) ? (lowerValue = initialChildren) : (lowerValue = processTableSize);
	
	for(int count = 0; count < lowerValue; count++) {
		pid_t newChild;
		newChild = fork();
		if(newChild < 0) {
			perror("Fork failed");
			exit(-1);
		}
		else if(newChild == 0) {
			char fakeArg[sizeof(int)];
			snprintf(fakeArg, sizeof(int), "%d", 1);
			execlp("./worker", fakeArg, NULL);
       		exit(1);
       		}
		else {
			initializePCB(newChild);
			printf("MASTER: Launching Child PID %d\n", newChild);
			runningChildren++;
		}
	}
}

void nonblockWait() {
	int status;
	pid_t terminatedChild = waitpid(0, &status, WNOHANG);

	if(terminatedChild <= 0)
		return;

	childTerminated(terminatedChild);
}

void childTerminated(pid_t terminatedChild) {
	for(int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
		while(release(terminatedChild, count));
	}

	processEnded(terminatedChild);//TODO reset checkChildren to test for occupied status and do away with runningChildren
	runningChildren--;
	printf("MASTER: Child pid %d has terminated and its resources have been released.\n", terminatedChild);
	//TODO: output to logfile that child terminated
}

void checkForMessages() {
	msgBuffer rcvbuf;
	if(msgrcv(msqid, &rcvbuf, sizeof(msgBuffer), 0, IPC_NOWAIT) == -1) {
   		if(errno == ENOMSG) {
      		//printf("Got no message so maybe do nothing?\n");
   		}
		else {
				printf("Got an error from msgrcv\n");
				perror("msgrcv");
				terminateProgram(6);
			}
	}
	else if(rcvbuf.childPid != 0) {
		printf("Received %d from worker pid %d\n",rcvbuf.intData, rcvbuf.childPid);
		takeAction(rcvbuf.childPid, rcvbuf.intData);
	}
}

void takeAction(pid_t childPid, int msgData) {
	if(msgData < 10) {
		request(childPid, msgData);
		return;
	}
	if(msgData < 20) {
		release(childPid, (msgData - RESOURCE_TABLE_SIZE)); //msgData will come back as the resource number + 10
		return;
	}
	childTerminated(childPid);
}

void request(pid_t childPid, int resourceNumber) {
	int entry = findTableIndex(childPid);
	processTable[entry].requestVector[resourceNumber] += 1; //TODO change this to enqueue
	printf("MASTER: Child pid %d has requested an instance of resource %d\n", childPid, resourceNumber);
	grantResource(childPid, resourceNumber, entry);
}

int release(pid_t childPid, int resourceNumber) {
	int entry = findTableIndex(childPid);
	if(processTable[entry].allocationVector[resourceNumber] > 0) {
		processTable[entry].allocationVector[resourceNumber] -= 1;
		printf("MASTER: Child pid %d has released an instance of resource %d\n", childPid, resourceNumber);
		resourceTable[resourceNumber].availableInstances += 1;
		sendMessage(childPid, 2);
		return 1;
	}
	printf("MASTER: Child pid %d has attempted to release an instance of resource %d that it does not have\n", childPid, resourceNumber);
	return 0;
}

//Tries to grant the most recent request first and sends a message back to that child.
//After, it attempts to grant any remaining requests in a first process in, first process out order
//NOTE: I know that this is not a very equitable way to do this, and it could lead to starvation.
void grantResource(pid_t childPid, int resourceNumber, int processNumber) {
	buf.mtype = childPid;
	if(resourceTable[resourceNumber].availableInstances > 0) {
		processTable[processNumber].allocationVector[resourceNumber] += 1;
		processTable[processNumber].requestVector[resourceNumber] -= 1;
		
		printf("MASTER: Requested instance of resource %d to child pid %d has been granted.\n", resourceNumber, childPid);
		sendMessage(childPid, 1);
	}
	else {
		printf("MASTER: Requested instance of resource %d to child pid %d has been denied.\n", resourceNumber, childPid);
		sendMessage(childPid, 0);
	}
}

void sendMessage(pid_t childPid, int msg) {
	buf.intData = msg;
	buf.mtype = childPid;
	printf("MASTER: Sending message of %d to child pid %d\n", msg, childPid);
	if(msgsnd(msqid, &buf, sizeof(msgBuffer) - sizeof(long), 0) == -1) {
			perror("msgsnd to child failed\n");
			terminateProgram(6);
	}
}

void checkTime(int *outputTimer, int *deadlockDetectionTimer) {
	if(abs(simulatedClock[1] - *outputTimer) >= HALF_SECOND){
			*outputTimer = simulatedClock[1];
			printf("\nOSS PID:%d SysClockS:%d SysClockNano:%d\n", getpid(), simulatedClock[0], simulatedClock[1]); 
			outputTable(fptr);
		}
	if(abs(simulatedClock[0] - *deadlockDetectionTimer) >= ONE_SECOND) {
		*deadlockDetectionTimer = simulatedClock[0];
		//runDeadlockDetection(); //TODO
	}
}

void help() {
    printf("This program is designed to simulate a process scheduler.\n");
	printf("The main program (is supposed to) launch child workers periodically and launch them based upon priority.\n");
	printf("The runtime of each worker is based upon a fixed integer, of which it may only use part of.\n");
	printf("The child processes will either use the whole time and not terminate, use part of the time and terminate, or use part of the time and go into a blocked queue.\n\n");
    printf("The executable takes four flags: [-n proc], [-s simul], [-t timelimit], and [-f logfile].\n");
    printf("The value of proc determines the total number of child processes to be produced.\n");
	printf("The value of simul determines the number of children that can run simultaneously.\n");
	printf("The value of timelimit determines how often new children may be launched, in nanoseconds.\n");
	printf("The file name provided will be used as a logfile to which this program outputs.\n");
	printf("\nMADE BY JACOB (JT) FOX\nOctober 31st, 2023\n");
	exit(1);
}

//sets all initial pid values to 0
void initializeProcessTable() {
	for(int count = 0; count < processTableSize; count++) {
		processTable[count].pid = 0;
	}
}

//initializes values of the pcb
void initializePCB(pid_t pid) {
	int index;
	index = 0;

	while(processTable[index].pid != 0)
		index++;

	processTable[index].occupied = 1;
	processTable[index].pid = pid;
	processTable[index].startTimeSeconds = simulatedClock[0];
	processTable[index].startTimeNano = simulatedClock[1];
	processTable[index].blocked = 0;
}

void initializeResourceTable() {
	for(int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
		resourceTable[count].availableInstances = 20;
		resourceTable[count].totalInstances = 20;
	}
}

//Checks to see if another child can be launched. If so, it launches a new child.
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime) {
	//If the user defined time interval has not been reached, return.
	if((simulatedClock[1] - *lastLaunchTime) < launchInterval)
		return;

	if(checkChildren(maxSimulChildren) && stillChildrenToLaunch()) {
		pid_t newChild;
		newChild = fork();
		if(newChild < 0) {
			perror("Fork failed");
			exit(-1);
		}
		else if(newChild == 0) {
			char fakeArg[sizeof(int)];
			snprintf(fakeArg, sizeof(int), "%d", 1);
			execlp("./worker", fakeArg, NULL);
       		exit(1);
       		}
		else {
			initializePCB(newChild);
			*lastLaunchTime = simulatedClock[1];
			printf("Launching Child.\n");
			outputTable();
			runningChildren++;
		}
	}
}

//Returns true if the number of currently running children is less than the max
int checkChildren(int maxSimulChildren) {
	if(runningChildren < maxSimulChildren)
		return 1;
	return 0;
}

//If the maximum number of children has not been reached, return true. Otherwise return false
int stillChildrenToLaunch() {
	if(processTable[processTableSize - 1].pid == 0) {
		return 1;
	}
	return 0;
}

//Returns 1 if any children are running. Returns 0 otherwise
int childrenInSystem() {
	for(int count = 0; count < processTableSize; count++) {
		if(processTable[count].occupied) {
			return 1;
		}
	}
	return 0;
}

//returns the buffer index corresponding to a given pid
int findTableIndex(pid_t pid) {
	for(int count = 0; count < processTableSize; count++) {
		if(processTable[count].pid == pid)
			return count;
	}
	return 0;
}

void incrementClock(int timePassed) {
	simulatedClock[1] += timePassed;
	if(simulatedClock[1] >= ONE_SECOND) {
		simulatedClock[1] -= ONE_SECOND;
		simulatedClock[0] += 1;
	}
}

void terminateProgram(int signum) {
	//Kills any remaining active child processes
	int count;
	for(count = 0; count < processTableSize; count++) {
		if(processTable[count].occupied)
			kill(processTable[count].pid, signum);
	}

	//Frees allocated memory
	free(processTable);
	processTable = NULL;

	// get rid of message queue
	if (msgctl(msqid, IPC_RMID, NULL) == -1) {
		perror("msgctl to get rid of queue in parent failed");
		exit(1);
	}

	//close the log file
	fclose(fptr);

	//detach from and delete memory
	shmdt(simulatedClock);
	shmctl(shm_id, IPC_RMID, NULL);

	printf("Program is terminating. Goodbye!\n");
	exit(1);
}

void sighandler(int signum) {
	printf("\nCaught signal %d\n", signum);
	terminateProgram(signum);
	printf("If you're seeing this, then bad things have happened.\n");
}

//updates the PCB of a process that has ended
void processEnded(int pidNumber) {
	int i;
	for(i = 0; i < processTableSize; i++) {
		if(processTable[i].pid == pidNumber) {
			processTable[i].occupied = 0;
			return;
		}
	}
}

void outputTable() {
	printf("%s\n%-15s %-15s %15s %15s %15s %15s\n", "Process Table:", "Entry", "Occupied", "PID", "StartS", "StartN", "Blocked");
	//printf("Process Table:\nEntry Occupied   PID\tStartS StartN\tServiceS\tServiceN\tWaitS\tWaitN\tBlocked\n");
	int i;
	for(i = 0; i < processTableSize; i++) {
		printf("%-15d %-15d %15d %15d %15d %15d\n\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startTimeSeconds, processTable[i].startTimeNano, processTable[i].blocked);
		fprintf(fptr, "%s\n%-15s %-15s %15s %15s %15s %15s\n", "Process Table:", "Entry", "Occupied", "PID", "StartS", "StartN", "Blocked");
		fprintf(fptr, "%-15d %-15d %15d %15d %15d %15d\n\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startTimeSeconds, processTable[i].startTimeNano, processTable[i].blocked);
	}
}

void sendingOutput(int chldNum, int chldPid, int systemClock[2]) {
	fprintf(fptr, "OSS:\t Sending message to worker %d PID %d at time %d:%d\n", chldNum, chldPid, systemClock[0], systemClock[1]);
}

void receivingOutput(int chldNum, int chldPid, int systemClock[2], msgBuffer rcvbuf) {
	if(rcvbuf.intData != 0) {
		fprintf(fptr, "OSS:\t Receiving message from worker %d PID %d at time %d:%d\n", chldNum, chldPid, systemClock[0], systemClock[1]);
	}
	else {
		printf("OSS:\t Worker %d PID %d is planning to terminate.\n", chldNum, chldPid);	
		fprintf(fptr, "OSS:\t Worker %d PID %d is planning to terminate.\n", chldNum, chldPid);	
	}
}

int addItemToQueue(pid_t *queue, pid_t itemToAdd) {
	for(int count = 0; count < processTableSize; count++) {
		if(queue[count] == -1) {
			queue[count] = itemToAdd;
			return 1;
		}
	}
	printf("queue full\n");
	return 0;
}

int removeItemFromQueue(pid_t *queue, pid_t itemToRemove) {
	for(int count = 0; count < processTableSize; count++) {
		if(queue[count] == itemToRemove) {
			queue[count] = -1;
			return 1;
		}
	}
	printf("pid not found in queue\n");
	return 0;
}

void initializeQueue(pid_t *queue) {
	int count;
	for(count = 0; count < processTableSize; count++) {
		queue[count] = -1;
	}
}