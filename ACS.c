#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

#define NCLERKS 5
#define QUEUE_MAX 30
#define ECONOMY 0
#define BUSINESS 1

struct customer_info{ /// use this struct to record the customer information read from customers.txt
    int user_id;
	int class_type;
	int arrival_time;
	int service_time;
};
 
static struct timeval init_time;
double wait_times[2];
int queue_length[2]; // Stores the real-time queue length information
int calling_clerk;
int chosen_cust;
pthread_mutex_t start_time_lock; // Used by helper to avoid race condition
pthread_mutex_t wait_time_lock; // Used when updating overall wait times for customers
pthread_mutex_t queue_lock; // Used when editing customer order queue
pthread_mutex_t queue_econ_lock; // Used when editing economy queue
pthread_mutex_t queue_biz_lock; // Used when editing business queue
pthread_mutex_t clerk_service_lock; // Used to stop 2 clerks from trying to service the same customer
sem_t calling_clerk_sem; // Used when passing calling clerk to customer
sem_t chosen_cust_sem; // Used when passing who is at the front of the line to customers
pthread_cond_t queue_econ = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_biz = PTHREAD_COND_INITIALIZER;
pthread_cond_t clerk_conds[NCLERKS];

// Defs for queue implementation
int track_queue[2][QUEUE_MAX];
int front[2];
int back[2];
int num_waiting[2];

// Helpers //

// Queue helpers
void enqueue(int data, int queue) {
    if(back[queue] == QUEUE_MAX-1) {
        back[queue] = -1;            
    }
    track_queue[queue][++back[queue]] = data;
	queue_length[queue]++;
}

int dequeue(int queue) {
   int data = track_queue[queue][front[queue]++];
   if(front[queue] == QUEUE_MAX) {
      front[queue] = 0;
   }
   queue_length[queue]--;
   return data;
}

// Casts str to POSITIVIE long whith error checking
// Returns -1 on error or if input is negative
long safe_str2long(char* str){
	char* tmp;
	errno = 0;
	long ret = strtol(str, &tmp, 10);
	if (errno || ret < 0)	return -1;
	return ret;
}

void safe_mutex_lock(pthread_mutex_t * mtx){
	if(pthread_mutex_lock(mtx) != 0){
		perror("Error locking mutex: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_mutex_unlock(pthread_mutex_t * mtx){
	if(pthread_mutex_unlock(mtx) != 0){
		perror("Error unlocking mutex: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_sem_wait(sem_t * sem){
	if(sem_wait(sem) != 0){
		perror("Error waiting on semaphore: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_sem_post(sem_t * sem){
	if(sem_post(sem) != 0){
		perror("Error posting semaphore: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_cond_wait(pthread_cond_t * cond, pthread_mutex_t * mtx){
	if(pthread_cond_wait(cond, mtx) != 0){
		perror("Error waiting on convar: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_cond_signal(pthread_cond_t * cond){
	if(pthread_cond_signal(cond) != 0){
		perror("Error signalling convar: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_cond_broadcast(pthread_cond_t * cond){
	if(pthread_cond_broadcast(cond) != 0){
		perror("Error broadcasting convar: ");
		exit(EXIT_FAILURE);
	}
	return;
}

void safe_usleep(__useconds_t time){
	if(usleep(time) != 0){
		perror("Error sleeping: ");
		exit(EXIT_FAILURE);
	}
}

double getCurrentSimulationTime(){
	struct timeval cur_time;
	double cur_secs, init_secs;
	
	safe_mutex_lock(&start_time_lock);
	init_secs = (init_time.tv_sec + (double) init_time.tv_usec / 1000000);
	safe_mutex_unlock(&start_time_lock);
	
	gettimeofday(&cur_time, NULL);
	safe_mutex_lock(&start_time_lock);
	cur_secs = (cur_time.tv_sec + (double) cur_time.tv_usec / 1000000);
	safe_mutex_unlock(&start_time_lock);

	return cur_secs - init_secs;
}
// End Helpers //


// Entry for customer threads
void * customer_entry(void * cus_info){
	double wait_start_time;
	struct customer_info * p_myInfo = (struct customer_info *) cus_info;
	safe_usleep(p_myInfo->arrival_time * 100000);
	fprintf(stdout, "A customer arrives: customer ID %2d. \n", p_myInfo->user_id);
	
	wait_start_time = getCurrentSimulationTime();
	pthread_cond_t * selected_queue_cond = (p_myInfo->class_type == ECONOMY) ? &queue_econ : &queue_biz;
	pthread_mutex_t * selected_queue_mtx = (p_myInfo->class_type == ECONOMY) ? &queue_econ_lock : &queue_biz_lock;

	safe_mutex_lock(selected_queue_mtx);
	
	safe_mutex_lock(&queue_lock);
	enqueue(p_myInfo->user_id, p_myInfo->class_type);	
	fprintf(stdout, "Customer %d enters a queue: %1d, this queue length is now %2d. \n", p_myInfo->user_id, p_myInfo->class_type, queue_length[p_myInfo->class_type]);
	safe_mutex_unlock(&queue_lock);

	// Wait for clerk to be available
	while(chosen_cust != p_myInfo->user_id){
		safe_cond_wait(selected_queue_cond, selected_queue_mtx);
	}
	// Find out the clerk that woke us
	int clerk = calling_clerk;
	safe_sem_post(&calling_clerk_sem);

	// Update wait time tracker
	double service_start_time = getCurrentSimulationTime();

	fprintf(stdout, "A clerk starts serving a customer: start time %.2f, the customer ID %2d, the clerk ID %1d. \n", service_start_time, p_myInfo->user_id, clerk);
	
	// Unblock other customers
	safe_mutex_unlock(&queue_lock);
	safe_sem_post(&chosen_cust_sem);
	safe_mutex_unlock(selected_queue_mtx);
	
	// Update wait time counter now that other threads unblocked
	safe_mutex_lock(&wait_time_lock);
	wait_times[p_myInfo->class_type] += service_start_time - wait_start_time;
	safe_mutex_unlock(&wait_time_lock);

	// We are in service, wait
	safe_usleep(p_myInfo->service_time * 100000);
	
	// Service done
	fprintf(stdout, "A clerk finishes serving a customer: end time %.2f, the customer ID %2d, the clerk ID %1d. \n", getCurrentSimulationTime(), p_myInfo->user_id, clerk);
	
	safe_cond_signal(&clerk_conds[(clerk-1)]); // Notify the clerk that service is finished, it can serve another customer
	
	pthread_exit(NULL);
	return NULL;
}

// Entry for clerk threads
void *clerk_entry(void * clerkNum){
	int * clerk_id = (int *) clerkNum;

	while(1){
		// clerk is idle now
		// If both queues are empty do nothing
		safe_mutex_lock(&clerk_service_lock);
		if((queue_length[0] == 0) && (queue_length[1] == 0)){
			safe_mutex_unlock(&clerk_service_lock);
			continue;
		}

		safe_mutex_lock(&queue_lock);
		pthread_cond_t * selected_queue_cond = (queue_length[BUSINESS] == 0) ? &queue_econ : &queue_biz;
		pthread_mutex_t * selected_queue_mtx = (queue_length[BUSINESS] == 0) ? &queue_econ_lock : &queue_biz_lock;
		safe_mutex_unlock(&queue_lock);
		
		safe_sem_wait(&chosen_cust_sem); // Semaphore unlocked after winning customer compares ID
		safe_mutex_lock(&queue_lock);
		if(queue_length[BUSINESS] != 0){
			chosen_cust = dequeue(BUSINESS);
		}
		else{
			chosen_cust = dequeue(ECONOMY);
		}
		safe_mutex_unlock(&queue_lock);

		safe_sem_wait(&calling_clerk_sem); // Semaphore unlocked after customer reads clerk id
		
		calling_clerk = *clerk_id;
		safe_cond_broadcast(selected_queue_cond); // Awake the customer (the one enter into the queue first)
		safe_mutex_unlock(&clerk_service_lock);
		
		safe_cond_wait(&clerk_conds[((*clerk_id)-1)], selected_queue_mtx); // wait the customer to finish its service, clerk busy
	}
	
	pthread_exit(NULL);
	
	return NULL;
}


int main(int argc, char *argv[]) {
	gettimeofday(&init_time, NULL); // record simulation start time
	long NCustomers = 0; // Defined after we read the first line of input
	long NEconomy = 0;
	long NBusiness = 0;
	FILE *fp;
	char * line = NULL;
    size_t len = 0;

	// Queue init
	front[0] = 0;
	front[1] = 0;
	back[0] = -1;
	back[1] = -1;

	// Initialize mutexes
	if (pthread_mutex_init(&start_time_lock, NULL) != 0 ||
		pthread_mutex_init(&wait_time_lock, NULL) != 0 ||
		pthread_mutex_init(&queue_lock, NULL) != 0 ||
		pthread_mutex_init(&queue_econ_lock, NULL) != 0 ||
		pthread_mutex_init(&queue_biz_lock, NULL) != 0 ||	
		pthread_mutex_init(&clerk_service_lock, NULL) != 0 ||
		sem_init(&calling_clerk_sem, 0, 1) != 0 ||
		sem_init(&chosen_cust_sem, 0, 1) != 0)
	{
		printf("\n Mutex init failed\n");
		exit(EXIT_FAILURE);
    }

	if (argc != 2){
		printf("Usage: ACS <file.txt>");
		exit(EXIT_FAILURE);
	}

	// Initialize clerk conds
	for(int i=0;i<NCLERKS;i++){
		if(pthread_cond_init(&clerk_conds[i], NULL) != 0){
			perror("Error initializing condvar: ");
			exit(EXIT_FAILURE);
		}
	}

	// Parse input and build needed data structures
	fp = fopen(argv[1], "r");
	if (fp == NULL){
		perror("Error opening file: ");
		exit(EXIT_FAILURE);
	}
		
	// Fist line has a different format, parse it seperately
	getline(&line, &len, fp);
	NCustomers = safe_str2long(line);
	if (NCustomers <= 0){
		printf("Invalid number of customers, check input file format.\n");
		exit(EXIT_FAILURE);
	}

	// Now declare customer info array with correct length
	struct customer_info cus_info[NCustomers];

	// Parse all customers into a struct array
	int i = 0;
	while ((getline(&line, &len, fp)) != -1) {
		// Bad input with N higher than the actual # of customers
		if(NCustomers < i-1){
			break;
		}
		int num_parsed = sscanf(line, "%d:%d,%d,%d", &cus_info[i].user_id, &cus_info[i].class_type, &cus_info[i].arrival_time, &cus_info[i].service_time);

		if (num_parsed != 4){
			printf("Error parsing customers, check input file format.\n");
			exit(EXIT_FAILURE);
		}
		// Check for illegal values
		if (cus_info[i].class_type < 0 ||  1 < cus_info[i].class_type){
			printf("Invalid class type in customer with ID %d\n", cus_info[i].user_id);
			exit(EXIT_FAILURE);
		}
		if (cus_info[i].arrival_time < 0){
			printf("Negative arrival time in customer with ID %d\n", cus_info[i].user_id);
			exit(EXIT_FAILURE);
		}

		// Set economy and business counters for average wait times
		(cus_info[i].class_type == 0) ? NEconomy++:NBusiness++;

		i++;
	}

	if(fclose(fp) != 0){
		perror("Error closing file: ");
		exit(EXIT_FAILURE);
	}

	// Create clerk threads
	pthread_t clerk_threads[NCLERKS];
	int clerk_ids[NCLERKS];
	for(int i = 0; i < NCLERKS; i++){
		clerk_ids[i] = i+1;
		if(pthread_create(&clerk_threads[i], NULL, clerk_entry, (void *)&clerk_ids[i]) != 0){
			printf("Error creating clerk thread.\n");
			exit(EXIT_FAILURE);
		}
	}
	
	// Create customer threads
	pthread_t cus_threads[NCustomers];
	for(int i = 0; i < NCustomers; i++){
		if(pthread_create(&cus_threads[i], NULL, customer_entry, (void *)&cus_info[i]) != 0){
			printf("Error creating customer thread.\n");
			exit(EXIT_FAILURE);
		}
	}
	// wait for all customer threads to terminate
	for(int i = 0; i < NCustomers; i++){
		if(pthread_join(cus_threads[i], NULL) != 0){
			perror("Error joining thread: ");
			exit(EXIT_FAILURE);
		}
	}
	
	// calculate the average waiting time of all customers
	// Edge case where no customers showed up for a class
	if(NBusiness == 0){
		wait_times[1] = 0;
		NBusiness = 1;
	}
	if(NEconomy == 0){
		wait_times[0] = 0;
		NEconomy = 1;
	}
	float avg_wait = (float) (wait_times[0] + wait_times[1]) / NCustomers;
	float econ_wait = (float) (wait_times[0]) / NEconomy;
	float biz_wait = (float) (wait_times[1]) / NBusiness;
	printf("The average wait time for all customers: %.2fs\n", avg_wait);
	printf("The average wait time for economy customers: %.2fs\n", econ_wait);
	printf("The average wait time for business customers: %.2fs\n", biz_wait);
	
	return 0;
}
