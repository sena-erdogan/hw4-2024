#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_LENGTH 2048

typedef struct {
        char * source;
        char * dest;
        int src_fd;
        int dest_fd;
        int numberofWorkers;
        char src_filename[256];
        char dest_filename[256];
        off_t file_size;
        int buffer_in;
        int buffer_out;
        pthread_mutex_t mutex;
        pthread_cond_t buffer_empty;
        pthread_cond_t buffer_full;
}
ThreadPool;

ThreadPool * thread_pool;

int total_files = 0;
int regular_files = 0;
int fifo_files = 0;
int directory = 0;
long int total_bytes = 0;
int sig = 0;

void signal_handler(int signal) {
        if (signal == SIGINT) {
                printf("\n\nReceived SIGINT\n");

                pthread_mutex_destroy( & thread_pool -> mutex);
                pthread_cond_destroy( & thread_pool -> buffer_empty);

                printf("All threads destroyed. Program terminating.\n\n");

                exit(EXIT_FAILURE);
        }
}

long int size(const char * filename) {

        FILE * file = fopen(filename, "rb");
        if (file == NULL) {
                return -1;
        }

        fseek(file, 0, SEEK_END);
        long fileSize = ftell(file);
        fclose(file);

        return fileSize;
}

void copyfile(char * src, char * dst) {

        struct stat st;
        if (lstat(src, & st) == 0) {
                if (S_ISFIFO(st.st_mode)) {

                        if (mkfifo(dst, st.st_mode) != 0) {
                                printf("Failed to create destination FIFO: %s\n", dst);
                                return;
                        }

                        int src_fd = open(src, O_RDONLY | O_NONBLOCK);

                        if (src_fd == -1) {
                                printf("Failed to open source FIFO: %s\n", src);
                                return;
                        }

                        int dest_fd = open(dst, O_WRONLY | O_NONBLOCK);

                        if (dest_fd == -1) {
                                // printf("Failed to open destination FIFO: %s\n", dest_file);
                                close(src_fd);
                                return;
                        }

                        char buffer[4096];
                        ssize_t bytes_read;
                        ssize_t bytes_written;
                        while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
                                bytes_written = write(dest_fd, buffer, bytes_read);

                                if (bytes_written == -1) {
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                continue;
                                        } else {
                                                printf("Failed to write to destination FIFO: %s\n", dst);
                                                break;
                                        }
                                }
                        }

                        if (bytes_read == -1) {
                                printf("Failed to read from source FIFO: %s\n", src);
                        }

                        close(src_fd);
                        close(dest_fd);
                } else {
                        int src_fd = open(src, O_RDONLY);
                        if (src_fd == -1) {
                                printf("Failed to open source file: %s\n", src);
                                return;
                        }

                        int dest_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
                        if (dest_fd == -1) {
                                printf("Failed to open destination file: %s\n", dst);
                                close(dest_fd);
                                return;
                        }

                        char buffer[4096];
                        ssize_t bytes_read;
                        ssize_t bytes_written;
                        while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
                                bytes_written = write(dest_fd, buffer, bytes_read);
                                if (bytes_written == -1) {
                                        printf("Failed to write to destination file: %s\n", dst);
                                        break;
                                }
                        }

                        if (bytes_read == -1) {
                                printf("Failed to read from source file: %s\n", src);
                        }

                        close(src_fd);
                        close(dest_fd);
                }
        } else {
                printf("Failed to stat file: %s\n", src);
        }
}

void copydir(char * source, char * dest) {
        DIR * dir = opendir(source);
        if (dir == NULL) {
                printf("Failed to open source directory: %s\n", source);
                pthread_exit(NULL);
        }

        struct dirent * entry;
        char src[MAX_LENGTH];
        char dst[MAX_LENGTH];
        while ((entry = readdir(dir)) != NULL) {

                if (strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0) {
                        continue;
                }
                total_bytes += size(src);
                snprintf(src, sizeof(src), "%s/%s", source, entry -> d_name);
                snprintf(dst, sizeof(dst), "%s/%s", dest, entry -> d_name);

                if (entry -> d_type == DT_DIR) {
                        mkdir(dst, 0777);
                        directory++;
                        copydir(src, dst);
                } else if (entry -> d_type == DT_REG) {

                        struct stat st;
                        if (lstat(src, & st) == 0) {

                                copyfile(src, dst);
                                char * extension = strrchr(src, '.');
                                if (strcmp(extension, ".fifo") == 0) {
                                        fifo_files++;
                                } else {
                                        regular_files++;
                                }

                                total_files++;
                        } else {
                                printf("Failed to stat file: %s\n", src);
                        }
                } else if (entry -> d_type == DT_FIFO) {
                        copyfile(src, dst);
                        fifo_files++;
                        total_files++;
                }
        }

        closedir(dir);

}

void * manager(void * arg) {
        ThreadPool * thread_pool = (ThreadPool * ) arg;

        char * source = thread_pool -> source;
        char * dest = thread_pool -> dest;

        copydir(source, dest);

        sig = thread_pool -> numberofWorkers;

        pthread_mutex_lock( & thread_pool -> mutex);
        pthread_cond_broadcast( & thread_pool -> buffer_full);
        pthread_mutex_unlock( & thread_pool -> mutex);

        pthread_exit(NULL);
}

void * worker(void * arg) {
        ThreadPool * thread_pool = (ThreadPool * ) arg;
        while (1) {
                ssize_t bytes_read = 0;
                ssize_t bytes_written = 0;
                char buffer[4096];

                pthread_cond_signal( & thread_pool -> buffer_empty);
                pthread_mutex_unlock( & thread_pool -> mutex);

                pthread_exit(NULL);
                while (sig != thread_pool -> numberofWorkers) {
                        while (bytes_written == 0) {
                                bytes_read = read(thread_pool -> src_fd, buffer, sizeof(buffer));
                                bytes_written = write(thread_pool -> dest_fd, buffer, bytes_read);
                                bytes_written = -1;
                                bytes_read = 0;
                        }

                }

                close(thread_pool -> src_fd);
                close(thread_pool -> dest_fd);

                pthread_exit(NULL);
        }

}

int main(int argc, char * argv[]) {
        if (argc != 5) {
                printf("Usage: ./MWCp bufferSize numberofWorkers sourceFile destinationFile\n");
                return 1;
        }

        int bufferSize = atoi(argv[1]);
        int numberofWorkers = atoi(argv[2]);
        char * sourceFile = argv[3];
        char * destinationFile = argv[4];
        char dest[MAX_LENGTH];

        char * source = (char * ) malloc(sizeof(sourceFile));
        char * temp = (char * ) malloc(sizeof(source));
        strcpy(source, sourceFile);

        temp = strtok(source, "/");

        while (temp != NULL) {
                strcpy(source, temp);
                temp = strtok(NULL, "/");
        }

        struct stat st;
        if (stat(destinationFile, & st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(dest, sizeof(dest), "%s/%s", destinationFile, source);
        } else {
                strncpy(dest, destinationFile, sizeof(dest) - 1);
                dest[sizeof(dest) - 1] = '\0';
        }

        if (stat(dest, & st) == -1) {
                if (mkdir(dest, 0777) != 0) {
                        printf("Failed to create destination directory: %s\n", dest);
                        return 1;
                }
        }

        strcpy(source, sourceFile);

        struct sigaction sa;
        memset( & sa, 0, sizeof(sa));
        sa.sa_handler = & signal_handler;
        sigaction(SIGINT, & sa, NULL);

        thread_pool = (ThreadPool * ) malloc(sizeof(ThreadPool));

        thread_pool -> buffer_in = 0;
        thread_pool -> buffer_out = 0;

        pthread_mutex_init( & thread_pool -> mutex, NULL);
        pthread_cond_init( & thread_pool -> buffer_empty, NULL);
        pthread_cond_init( & thread_pool -> buffer_full, NULL);

        thread_pool -> source = (char * ) malloc(sizeof(source));
        strcpy(thread_pool -> source, source);
        thread_pool -> dest = (char * ) malloc(sizeof(dest));
        strcpy(thread_pool -> dest, dest);
        thread_pool -> numberofWorkers = numberofWorkers;

        pthread_t manager_thread;
        pthread_create( & manager_thread, NULL, manager, (void * ) thread_pool);

        pthread_t worker_threads[numberofWorkers];
        for (int i = 0; i < numberofWorkers; i++) {
                pthread_create( & worker_threads[i], NULL, worker, (void * ) thread_pool);
        }

        struct timeval start_time;
        gettimeofday( & start_time, NULL);

        pthread_join(manager_thread, NULL);

        for (int i = 0; i < numberofWorkers; i++) {
                pthread_join(worker_threads[i], NULL);
        }

        struct timeval end_time;
        gettimeofday( & end_time, NULL);

        double elapsed_time = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec) / 1000000.0;

        long minutes = (long) elapsed_time / 60;
        long seconds = (long) elapsed_time % 60;
        long milliseconds = (elapsed_time - (long) elapsed_time) * 1000;

        printf("\n---------------STATISTICS--------------------\n");
        printf("Consumers: %d - Buffer Size: %d\n", numberofWorkers, bufferSize);
        printf("Number of Regular File: %d\n", regular_files);
        printf("Number of FIFO File: %d\n", fifo_files);
        printf("Number of Directory: %d\n", directory);
        printf("TOTAL BYTES COPIED: %ld\n", total_bytes);
        printf("TOTAL TIME: %02ld:%02ld.%03ld (min:sec.mili)\n", minutes, seconds, milliseconds);

        printf("%s directory copied to %s directory successfully.\n", source, dest);

        free(temp);
        free(source);
        free(thread_pool -> source);
        free(thread_pool -> dest);
        pthread_mutex_destroy( & thread_pool -> mutex);
        pthread_cond_destroy( & thread_pool -> buffer_empty);
        pthread_cond_destroy( & thread_pool -> buffer_full);

        return 0;
}
