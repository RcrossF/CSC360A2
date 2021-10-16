#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

#define NQUEUES 2
#define NCLERKS 5

struct customer_info{ /// use this struct to record the customer information read from customers.txt
    int user_id;
	int class_type;
	int arrival_time;
	int service_time;
};
 
static struct timeval init_time;
double wait_times[NQUEUES];
int queue_length[NQUEUES];// variable stores the real-time queue length information; mutex_lock needed
pthread_mutex_t start_time_lock;
pthread_mutex_t wait_time_lock;
pthread_mutex_t queue_len_lock;
pthread_mutex_t queue_econ_lock;
pthread_mutex_t queue_biz_lock;
pthread_cond_t queue_econ = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_biz = PTHREAD_COND_INITIALIZER;


int main(int argc, char *argv[]) {
	gettimeofday(&init_time, NULL); // record simulation start time

	long NCustomers; // Defined after we read the first line of input
	FILE *fp;
	char * line = NULL;
    size_t len = 0;

	// Initialize mutexes
	if (pthread_mutex_init(&start_time_lock, NULL) != 0 &&
		pthread_mutex_init(&wait_time_lock, NULL) != 0 &&
		pthread_mutex_init(queue_len_lock, NULL) != 0 &&
		pthread_mutex_init(&queue_econ_lock, NULL) != 0 &&
		pthread_mutex_init(&queue_biz_lock, NULL) != 0)
	{
			printf("\n Mutex init failed\n");
			exit(EXIT_FAILURE)
    }

	if (argc != 2){
		printf("Usage: ACS <file.txt>");
		exit(EXIT_FAILURE);
	}
	else{
		// Parse input and build needed data structures
		fp = fopen(argv[1], "r");
		if (fp == NULL)
			printf("Error opening file. Check that it exists.");
			exit(EXIT_FAILURE);

		// Fist line has a different format, parse it seperately
		getline(&line, &len, fp)
		NCustomers = safe_str2long(line);
		if (NCustomers <= 0){
			printf("Invalid number of customers, check input file format.");
			exit(EXIT_FAILURE);
		}

		// Now declare customer info array with correct length
		struct customer_info cus_info[NCustomers];

		// Parse all customers into a struct array
		int i = 0;
		while ((getline(&line, &len, fp)) != -1) {
			int num_parsed = sscanf("%d:%d,%d,%d", &cus_info[i].user_id, &cus_info[i].class_type, &cus_info[i].arrival_time, &cus_info[i].service_time);

			if (num_parsed != 4){
				printf("Error parsing customers, check input file format");
				exit(EXIT_FAILURE);
			}
			// Check for illegal values
			if (cus_info[i].class_type < 0 ||  1 < cus_info[i].class_type){
				printf("Invalid class type in customer with ID %d", cus_info[i].user_id);
				exit(EXIT_FAILURE);
			}
			if (cus_info[i].arrival_time < 0){
				printf("Negative arrival time in customer with ID %d", cus_info[i].user_id);
				exit(EXIT_FAILURE);
			}
			i++;
		}

    fclose(fp);
	}
	// initialize all the condition variable and thread lock will be used
	
	
	/** Read customer information from txt file and store them in the structure you created 
		
		1. Allocate memory(array, link list etc.) to store the customer information.
		2. File operation: fopen fread getline/gets/fread ..., store information in data structure you created

	*/
	// Create clerk threads
	pthread_t clerk_threads[NCLERKS];
	for(i = 0, i < NCLERKS; i++){
		if(pthread_create(&clerk_threads[i], NULL, clerk_entry, (void *)&clerk_info[i]) != 0){
			printf("Error creating clerk thread.");
			exit(EXIT_FAILURE);
		}
	}
	
	// Create customer threads
	pthread_t cus_threads[NCustomers];
	for(i = 0, i < NCustomers; i++){
		if(pthread_create(&cus_threads[i], NULL, customer_entry, (void *)&cus_info[i]) != 0){
			printf("Error creating customer thread.");
			exit(EXIT_FAILURE);
		}
	}
	// wait for all customer threads to terminate
	for(i = 0, i < NCustomers; i++){
		pthread_join(cus_threads[i]);
	}
	// wait for all clerk threads to terminate
	for(i = 0, i < NCLERKS; i++){
		pthread_join(clerk_threads[i]);
	}

	pthread_mutex_destroy(&wait_time_lock);
	pthread_mutex_destroy(&queue_len_lock);
	pthread_mutex_destroy(&queue_econ_lock);
	pthread_mutex_destroy(&queue_biz_lock);
	pthread_cond_destroy(&queue_econ);
	pthread_cond_destroy(&queue_biz);
	
	// calculate the average waiting time of all customers
	return 0;
}

// function entry for customer threads
void * customer_entry(void * cus_info){
	struct customer_info * p_myInfo = (struct customer_info *) cus_info;
	
	usleep(p_myInfo.arrival_time);
	fprintf(stdout, "A customer arrives: customer ID %2d. \n", p_myInfo.user_id);
	
	double wait_start_time = getCurrentSimulationTime();
	if (p_myInfo.class_type == 1){
		pthread_mutex_lock(&queue_biz_lock);
	}
	else {
		pthread_mutex_lock(&queue_econ_lock);
	}
	
	queue_length[p_myInfo.class_type]++;
	fprintf(stdout, "A customer enters a queue: %1d, this queue length is now %2d. \n", p_myInfo.class_type, queue_length[p_myInfo.class_type]);
	
	if (p_myInfo.class_type == 1){
		if(pthread_cond_wait(&queue_biz, &queue_biz_lock) != 0){
			printf("Thread for customer %d error waking.", p_myInfo.user_id);
			exit(EXIT_FAILURE);
		}
		pthread_mutex_unlock(&queue_biz_lock);
	}
	else {
		if(pthread_cond_wait(&queue_econ, &queue_econ_lock) != 0){
			printf("Thread for customer %d error waking.", p_myInfo.user_id);
			exit(EXIT_FAILURE);
		}
		pthread_mutex_unlock(&queue_econ_lock);
	}
	
	/* Try to figure out which clerk awoken me, because you need to print the clerk Id information */
	
	// Update wait time tracker
	double service_start_time = getCurrentSimulationTime();
	pthread_mutex_lock(&wait_time_lock);
	wait_times[p_myInfo.class_type] += service_start_time - wait_start_time;
	pthread_mutex_unlock(&wait_time_lock);

	fprintf(stdout, "A clerk starts serving a customer: start time %.2f, the customer ID %2d, the clerk ID %1d. \n", service_start_time, p_myInfo.user_id, -1);
	
	usleep(p_myInfo.service_time);
	
	double service_end_time = getCurrentSimulationTime()
	fprintf(stdout, "A clerk finishes serving a customer: end time %.2f, the customer ID %2d, the clerk ID %1d. \n", service_end_time, p_myInfo.user_id, -1);\
	
	pthread_cond_signal(/* The clerk awoken me */); // Notify the clerk that service is finished, it can serve another customer
	
	pthread_exit(NULL);
	
	return NULL;
}

// function entry for clerk threads
void *clerk_entry(void * clerkNum){
	
	while(TRUE){
		// clerk is idle now

		pthread_mutex_lock(&queue_len_lock);		
		pthread_cond_t * selected_queue_cond = (queue_length[1] == 0) ? queue_econ : queue_biz;
		pthread_mutex_t * selected_queue_mtx = (queue_length[1] == 0) ? queue_econ_lock : queue_biz_lock;
		pthread_mutex_unlock(&queue_len_lock);
		
		pthread_mutex_lock(&selected_queue_mtx);
		pthread_cond_broadcast(&selected_queue_cond); // Awake the customer (the one enter into the queue first) from the longest queue (notice the customer he can get served now)
		pthread_mutex_unlock(&selected_queue_mtx);
		
		pthread_cond_wait(); // wait the customer to finish its service, clerk busy
	}
	
	pthread_exit(NULL);
	
	return NULL;
}

// Casts str to POSITIVIE long whith error checking
// Returns -1 on error or if input is negative
long safe_str2long(char* str){
	char* tmp;
	errno = 0;
	long ret = strtol(line, &tmp, 10);
	if (errno || ret < 0)	return -1;
}

double getCurrentSimulationTime(){
	struct timeval cur_time;
	double cur_secs, init_secs;
	
	pthread_mutex_lock(&start_time_mtex);
	init_secs = (init_time.tv_sec + (double) init_time.tv_usec / 1000000);
	pthread_mutex_unlock(&start_time_mtex);
	
	gettimeofday(&cur_time, NULL);
	cur_secs = (cur_time.tv_sec + (double) cur_time.tv_usec / 1000000);
	
	return cur_secs - init_secs;
}