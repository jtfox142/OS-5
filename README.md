Link to GitHub: https://github.com/jtfox142/OS-5

This program simulates resource management and the implementation of a deadlock detection algorithm. A process table and a resource table are created and maintained by OSS.
Processes are launched in accordance to the arguments provided by the user, as described by the help message provided via the -h flag. These arguments have not changed in
any significant way from the prior project, OS-4, except that up to simul number of worker processes are launched immediately. 

There are 10 resources total and 20 instances of each resource. This does not change. As mentioned above, these resources are maintained in a resource table in OSS.
When a worker requests a resource, a request is placed in the request vector of the corresponding process. If the request is granted, then the number of available instances
of that resource in the resource table is decremented. Within the process table for that worker, the request vector is decremented back to zero and the allocation vector slot for
that specific resource is incremented.

A worker process can request a resource, release a resource, or, if at least one second has passed, possibly terminate itself if it has been allotted all of its requested resources.
Workers are not allowed to request more instances of a resource than the maximum of 20. If it tries to, then it is terminated. If a request for a resource is denied, 
the worker "goes to sleep". It will not do anything else until it receives a message from OSS granting the requested resource. It is effectively blocked.

Once every second, OSS will run a deadlock detection algorithm that constructs a request matrix, allocation matrix, and available vector to calculate if deadlock has/will occur.
If deadlock has/will occur, then the process that is currently using the largest number of resources is killed. I chose to do it this way because it provides the best odds of deadlock
being solved on the first try.

I have already commented this three times inside of OSS but I want to make it blatently obvious as to avoid plagerism: I copied the code for the queue structure and queue functions from
https://www.geeksforgeeks.org/introduction-and-array-implementation-of-queue/. Nothing else in my code was copied from any source or any person. Everything else in my code is of my own
creation.

I do not know of any problems in this code that would cause memory leakages or unsafe behavior. In my tests it runs as it should, terminates as it should, and does the thing it is supposed
to do. I have run it many times using ./oss -n 3 -s 2 -t 4 -f logfile.txt

The only ways in which I can specifically recall the implementation of my project differing from the specification is that I did not enforce a 10k line limit on output to the logfile,
and I did not offer a verbose on/off option for the user. Sorry.

MADE BY JACOB (JT) FOX, November 20th, 2023
