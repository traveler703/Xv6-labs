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

#define BUCK_SIZ 13
#define BCACHE_HASH(dev, blk) (((dev << 27) | blk) % BUCK_SIZ) // 支持多个 devv

struct {
  struct spinlock bhash_lk[BUCK_SIZ]; // buf hash lock
  struct buf bhash_head[BUCK_SIZ]; // 每个桶的开头，不用buf*是因为我们需要得到某个buf前面的buf

  struct buf buf[NBUF]; // 最终的缓存

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

void
binit(void)
{
  for (int i = 0; i < BUCK_SIZ; i++){
    initlock(&bcache.bhash_lk[i], "bcache buf hash lock");
    bcache.bhash_head[i].next = 0;
  }

  for(int i = 0; i < NBUF; i++){ // 最开始把所有缓存都分配到桶 0 上
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buf sleep lock");
    b->lst_use = 0;
    b->refcnt = 0;
    b->next = bcache.bhash_head[0].next; // 往 0 的头上插
    bcache.bhash_head[0].next = b;
  }
}

struct buf* bfind_prelru(int* lru_bkt){ // 返回 lru 前面的一个，并且加锁
  struct buf* lru_res = 0;
  *lru_bkt = -1;
  struct buf* b;
  for(int i = 0; i < BUCK_SIZ; i++){
    acquire(&bcache.bhash_lk[i]);
    int found_new = 0;
    for(b = &bcache.bhash_head[i]; b->next; b = b->next){ 
      if(b->next->refcnt == 0 && (!lru_res || b->next->lst_use < lru_res->next->lst_use)){
        lru_res = b;
        found_new = 1;
      }
    }
    if(!found_new){
      // 没有更好的选择，就一直持有这个锁（需要确保一直持有最佳选择对应桶的锁）
      release(&bcache.bhash_lk[i]);
    }else{ // 有更好的选择（有更久没使用的）
      if(*lru_bkt != -1) release(&bcache.bhash_lk[*lru_bkt]); // 直接释放以前选择的锁
      *lru_bkt = i; // 更新最佳选择
    }
  }
  return lru_res;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint key = BCACHE_HASH(dev, blockno);
  acquire(&bcache.bhash_lk[key]);

  // Is the block already cached?
  for(b = bcache.bhash_head[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bhash_lk[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bhash_lk[key]);
  int lru_bkt;
  struct buf* pre_lru = bfind_prelru(&lru_bkt);
  // pre_lru 会返回空闲缓存前一个（链表中前一个）缓存的地址
  // 并且确保拿到了缓存对应的桶锁
  // 我们会传进去一个 lru_bkt，函数执行好后，这个值会储存缓存对应的桶
  if(pre_lru == 0){
    panic("bget: no buffers");
  }
  struct buf* lru = pre_lru->next; 
  // lru （lru 是最久没有使用的缓存，并且 refcnt = 0）是 pre_lru 后面的一个
  pre_lru->next = lru->next; 
  // 让 pre_lru 的后面一个直接变成 lru 的后面一个，相当于删除 lru
  release(&bcache.bhash_lk[lru_bkt]);
  acquire(&bcache.bhash_lk[key]);  
  for(b = bcache.bhash_head[key].next; b; b = b->next){
    // 拿到锁之后要确保没有重复添加缓存
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bhash_lk[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  lru->next = bcache.bhash_head[key].next; // 把找到的缓存添加到链表头部
  bcache.bhash_head[key].next = lru;

  lru->dev = dev, lru->blockno = blockno;
  lru->valid = 0, lru->refcnt = 1; 

  release(&bcache.bhash_lk[key]);

  acquiresleep(&lru->lock);
  return lru;
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

  uint key = BCACHE_HASH(b->dev, b->blockno);
  // 改成散列表后要先得到 key
  acquire(&bcache.bhash_lk[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->lst_use = ticks;
  }
  
  release(&bcache.bhash_lk[key]);
}

void
bpin(struct buf *b) {
  uint key = BCACHE_HASH(b->dev, b->blockno);
  acquire(&bcache.bhash_lk[key]);
  b->refcnt++;
  release(&bcache.bhash_lk[key]);
}

void
bunpin(struct buf *b) {
  uint key = BCACHE_HASH(b->dev, b->blockno);
  acquire(&bcache.bhash_lk[key]);
  b->refcnt--;
  release(&bcache.bhash_lk[key]);
}


