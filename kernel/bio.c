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
#define BUFMAP_HASH(dev, blockno) ((((dev)<<27)|(blockno))%NBUFMAP_BUCKET)

struct {
  struct spinlock eviction_lock;          // 乐观锁
  struct buf buf[NBUF];

  struct buf bufmap[NBUFMAP_BUCKET];      // 桶
  struct spinlock bufmap_locks[NBUFMAP_BUCKET]; // 桶锁
} bcache;

void
binit(void)
{
  for(int i = 0; i < NBUFMAP_BUCKET; i++) {
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
    bcache.bufmap[i].next = 0;
  }

  // Initialize buffers
  for(int i = 0; i < NBUF; i++){
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->lastuse = 0;
    b->refcnt = 0;
    // put all the buffers into bufmap[0]
    b->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = b;
  }

  initlock(&bcache.eviction_lock, "bcache_eviction");

}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint key = BUFMAP_HASH(dev, blockno);

  // 获取待查找 key 桶下的锁
  acquire(&bcache.bufmap_locks[key]);

  // 判断是否在缓存中，如果在就直接返回
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 释放 key 桶锁，防止死锁，破坏请求和保持
  release(&bcache.bufmap_locks[key]);

  // 防止创建重复缓存的情况，也就是不会发送驱逐或重用
  acquire(&bcache.eviction_lock);

  // 再次检查，判断是否已经被缓存，否则如果中间被其他线程调用驱逐锁一直得不到释放
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      acquire(&bcache.bufmap_locks[key]); // must do, for `refcnt++`
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 没有被缓存的情况
  // 目前只持有驱逐锁
  // 遍历所有桶找到 LRU 并锁定对应桶
  struct buf *before_least = 0; 
  uint holding_bucket = -1;
  for(int i = 0; i < NBUFMAP_BUCKET; i++){
    acquire(&bcache.bufmap_locks[i]);
    int newfound = 0; 
    // 寻找该桶下的 LRU cache
    for(b = &bcache.bufmap[i]; b->next; b = b->next) {
      if(b->next->refcnt == 0 && 
      (!before_least || b->next->lastuse < before_least->next->lastuse)) {
        before_least = b;
        newfound = 1;
      }
    }
    // 如果没有找到那么直接释放该桶的锁
    if(!newfound) {
      release(&bcache.bufmap_locks[i]);
    } else {
      if(holding_bucket != -1) release(&bcache.bufmap_locks[holding_bucket]);
      holding_bucket = i;
    }
  }

  if(!before_least) {
    panic("bget: no buffers");
  }
  b = before_least->next;

  if(holding_bucket != key) {
    // remove the buf from it's original bucket
    before_least->next = b->next;
    release(&bcache.bufmap_locks[holding_bucket]);
    // rehash and add it to the target bucket
    acquire(&bcache.bufmap_locks[key]);
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }
  
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  release(&bcache.bufmap_locks[key]);
  release(&bcache.eviction_lock);
  acquiresleep(&b->lock);
  return b;
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

  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->lastuse = ticks;
  }
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}


