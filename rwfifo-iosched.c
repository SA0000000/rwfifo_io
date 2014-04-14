/*
  * elevator Read-Write FIFO
  */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
 
 static const int writes_starved = 2;    /* max times reads can starve a write */
 static const int fifo_batch = 16;       /* # of sequential requests treated as one by the above parameters. For throughput. */
 static const int max_reads = 3;
 static const int max_writes = 2;
										//fs.h defines READ 0 and WRITE 1
 struct rwfifo_data {
         struct list_head fifo_list[2];

         struct request *next_rq[2];
         unsigned int batching;          /* number of sequential requests made */
         sector_t last_sector;           /* head position */
         unsigned int starved;           /* times reads have starved writes */
         
         /* settings that change how the i/o scheduler behaves */
         int fifo_batch;
         int writes_starved;
         int front_merges;
         int max_reads;
         int max_writes;
         int read_count;
         int write_count;
 };
 
 static void rwfifo_merged_requests(struct request_queue *q, struct request *rq, struct request *next)
 {
         list_del_init(&next->queuelist);
 }
 
 
 //to remove a request from a list
 static void rwfifo_remove_request(struct request_queue *q, struct request *rq)
 {
 		struct deadline_data *dd = q->elevator->elevator_data;
		rq_fifo_clear(rq);
 }
 
 static void rwfifo_move_to_dispatch(struct rwfifo_data *rwd,struct request *rq)
 {
 		 struct	request_queue *q = rq->q;
 		 
 		 rwfifo_remove_request(q,rq);
 		 elv_dispatch_add_tail(q,rq);
 }
 
 //dispatch read and write requests
 static int rwfifo_dispatch(struct request_queue *q, int force)
 {
         struct rwfifo_data *rwd = q->elevator->elevator_data;
         const int reads=!list_empty(&rwd->fifo_list[READ]);		//list_empty returns 0 if list is non-empty
 		 const int writes =!list_empty(&rwd->fifo_list[WRITE]);
 		 struct request *rq;
 		 int data_dir;
		  //while there are both read and write requests dispatch 60% reads and 40% writes 
		 //that is for every three reads there are two writes
         if (reads){
         		//if the number of reads in less than the maximum number of reads
		 		//and if there  are no write requests then dispatch read requests
		 	if( (rwd->read_count++ < rwd->max_reads)) || !writes){
		 	   rwd->data_dir=READ;
		 	   rwd->write_count=0;
		 	}
         	else{	
			   data_dir=WRITE;
			   rwd->write_count++;
			   //if the number of write requests dispatched is greater than max_writes, then start dispatching read requests
			   if(rwd->write_count >= rwd->max_writes)		
			   	  rwd->read_count=0;
         	}
         }
         else if(writes)   //there are no read requests but there are write requests
         {
         	data_dir=WRITE;
         	rwd->write_count++;
         	rwd->read_count=0;
         }
         
         rq=rq_entry_fifo(rwd->fifo_list[data_dir].next;)
         
         rwfifo_move_to_dispatch(rwd,rq);
        
         //list_del_init(&rq->queuelist);
            //elv_dispatch_sort(q, rq);
         return 1;
 }
 
 //Add request to respective fifo list either read or write
 static void rwfifo_add_request(struct request_queue *q, struct request *rq)
 {
         struct rwfifo_data *rwd = q->elevator->elevator_data;
 		 const int data_dir = rq_data_dir(rq);
         list_add_tail(&rq->queuelist, &rwd->fifo_list[data_dir]);
         
         //-----------------------------------------------------//

		/*set expire time and add to fifo list*/  //rq_set_fifo_time(rq, jiffies + dd->fifo_expire[data_dir]);
 }
 
 //to check if both the read and write queues are empty or not
 static int rwfifo_queue_empty(struct request_queue *q)
 {
         struct rwfifo_data *rwd = q->elevator->elevator_data;
         
         return list_empty(&rwd->fifo_list[WRITE]) && list_empty(&rwd->fifo_list[READ]);
 }
 
 //Find the previous node in a sector-sorted order??????????????
 static struct request * rwfifo_former_request(struct request_queue *q, struct request *rq)
 {
         struct rwfifo_data *rwd = q->elevator->elevator_data;
 		 const int data_dir = rq_data_dir(rq);
         if (rq->queuelist.prev == &rwd->fifo_list[data_dir])
                return NULL;
         return list_entry(rq->queuelist.prev, struct request, queuelist);
 }
 
 //Find the next node in a sector-sorted order??????????????
 static struct request * rwfifo_latter_request(struct request_queue *q, struct request *rq)
 {
         struct rwfifo_data *rwd = q->elevator->elevator_data;
 		 const int data_dir = rq_data_dir(rq);
 		 
         if (rq->queuelist.next == &rwd->fifo_list[data_dir])
                 return NULL;
         return list_entry(rq->queuelist.next, struct request, queuelist);
 }
 
 //initialize read and write list heads
  static void * rwfifo_init_queue(struct request_queue *q)
  {
         struct rwfifo_data *rwd;
         rwd = kmalloc_node(sizeof(*rwd), GFP_KERNEL, q->node);
         if (!rwd)
            return NULL;
         INIT_LIST_HEAD(&rwd->fifo_list[READ]);
         INIT_LIST_HEAD(&rwd->fifo_list[WRITE]);

         rwd->writes_starved = writes_starved;
         rwd->front_merges = 1;
         rwd->fifo_batch = fifo_batch;
         rwd->max_reads = max_reads;
         rwd->max_writes= max_writes;
         rwd->read_count=0;
         rwd->write_count=0;
         return rwd;
  }
  
  //free the queues from memory
  static void rwfifo_exit_queue(struct elevator_queue *e)
  {
          struct rwfifo_data *rwd = e->elevator_data;
  
          BUG_ON(!list_empty(&rwd->fifo_list[READ]));
          BUG_ON(!list_empty(&rwd->fifo_list[WRITE]));
          kfree(rwd);
  }
  
  static struct elv_fs_entry rwfifo_attrs[] = {
       RWFIFO_ATTR(read_expire),
       RWFIFO_ATTR(write_expire),
       RWFIFO_ATTR(writes_starved),
       RWFIFO_ATTR(front_merges),
       RWFIFO_ATTR(fifo_batch),
       __ATTR_NULL
};
 
  static struct elevator_type iosched_rwfifo = {
          .ops = {
          		  .elevator_merge_fn              = rwfifo_merge,
                  .elevator_merge_req_fn          = rwfifo_merged_requests,
                  .elevator_dispatch_fn           = rwfifo_dispatch,
                  .elevator_add_req_fn            = rwfifo_add_request,		//done
                  .elevator_queue_empty_fn        = rwfifo_queue_empty,		//done
                  .elevator_former_req_fn         = rwfifo_former_request,	// modified 
                  .elevator_latter_req_fn         = rwfifo_latter_request,	// modified
                  .elevator_init_fn               = rwfifo_init_queue,		//done
                  .elevator_exit_fn               = rwfifo_exit_queue,		//done
         },
         .elevator_attrs = rwfifo_attrs,
         .elevator_name = "rwfifo",
         .elevator_owner = THIS_MODULE,
 };
 
 static int __init rwfifo_init(void)
 {
         elv_register(&iosched_rwfifo);
 
         return 0;
 }
 
 static void __exit rwfifo_exit(void)
 {
         elv_unregister(&iosched_rwfifo);
 }
 
 module_init(rwfifo_init);
 module_exit(rwfifo_exit);
 
 
 MODULE_AUTHOR("Sarah J. Andrabi");
 MODULE_LICENSE("GPL");
 MODULE_DESCRIPTION("Read-Write FIFO IO scheduler");
 
