// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct buf buf[NBUF];
} bcache;

#define NBUCKET 13
#define HASH(blockno) (blockno % NBUCKET)

struct {
  struct spinlock lock;
  struct buf head;
} hashtable[NBUCKET];

void
binit(void)
{
  struct buf *b;
  char lockname[16];

  for(int i = 0; i < NBUCKET; i++) {
    snprintf(lockname, sizeof(lockname), "bcache%d", i);
    initlock(&hashtable[i].lock, lockname);
    hashtable[i].head.prev = &hashtable[i].head;
    hashtable[i].head.next = &hashtable[i].head;
  }

  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // Put all buffers in bucket 0 initially
    b->next = hashtable[0].head.next;
    b->prev = &hashtable[0].head;
    initsleeplock(&b->lock, "buffer");
    hashtable[0].head.next->prev = b;
    hashtable[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int id = HASH(blockno);

  acquire(&hashtable[id].lock);

  // Is the block already cached?
  for(b = hashtable[id].head.next; b != &hashtable[id].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&hashtable[id].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = hashtable[id].head.prev; b != &hashtable[id].head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&hashtable[id].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  release(&hashtable[id].lock);
  
  // Steal from other buckets.
  for(int i = 0; i < NBUCKET; i++) {
     if (i == id) continue;
     
     acquire(&hashtable[i].lock);
     for(b = hashtable[i].head.prev; b != &hashtable[i].head; b = b->prev){
        if(b->refcnt == 0) {
           b->dev = dev;
           b->blockno = blockno;
           b->valid = 0;
           b->refcnt = 1;
           
           b->next->prev = b->prev;
           b->prev->next = b->next;
           release(&hashtable[i].lock);
           
           acquire(&hashtable[id].lock);
           struct buf *b2;
           for(b2 = hashtable[id].head.next; b2 != &hashtable[id].head; b2 = b2->next){
              if(b2->dev == dev && b2->blockno == blockno){
                 b2->refcnt++;
                 
                 b->next = hashtable[id].head.next;
                 b->prev = &hashtable[id].head;
                 hashtable[id].head.next->prev = b;
                 hashtable[id].head.next = b;
                 b->refcnt = 0; 
                 
                 release(&hashtable[id].lock);
                 acquiresleep(&b2->lock);
                 return b2;
              }
           }
           
           b->next = hashtable[id].head.next;
           b->prev = &hashtable[id].head;
           hashtable[id].head.next->prev = b;
           hashtable[id].head.next = b;
           release(&hashtable[id].lock);
           acquiresleep(&b->lock);
           return b;
        }
     }
     release(&hashtable[i].lock);
  }
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int id = HASH(b->blockno);
  acquire(&hashtable[id].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = hashtable[id].head.next;
    b->prev = &hashtable[id].head;
    hashtable[id].head.next->prev = b;
    hashtable[id].head.next = b;
  }
  
  release(&hashtable[id].lock);
}

void
bpin(struct buf *b) {
  int id = HASH(b->blockno);
  acquire(&hashtable[id].lock);
  b->refcnt++;
  release(&hashtable[id].lock);
}

void
bunpin(struct buf *b) {
  int id = HASH(b->blockno);
  acquire(&hashtable[id].lock);
  b->refcnt--;
  release(&hashtable[id].lock);
}


