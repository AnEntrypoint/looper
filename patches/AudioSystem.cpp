
#include "Audio.h"
#include <circle/logger.h>
#include <circle/alloc.h>
#ifdef ARM_ALLOW_MULTI_CORE
#include <circle/types.h>
extern void coreDispatchPush (u32 code);
#define DISPATCH_AUDIO 1u
#endif

#define log_name "audio"

#define AUDIO_RESERVE_MEMORY  4000000
    // reserve 4MB 


AudioCodec *AudioCodec::s_pCodec = 0;

u16  AudioSystem::s_numStreams = 0;
u32  AudioSystem::s_totalBlocks = 0;
u32  AudioSystem::s_blocksUsed = 0;
u32  AudioSystem::s_blocksUsedMax = 0;
u32  AudioSystem::s_cpuCycles = 0;
u32  AudioSystem::s_nInUpdate = 0;
u32  AudioSystem::s_cpuCyclesMax = 0;
u32  AudioSystem::s_numOverflows = 0;
bool AudioSystem::s_bUpdateScheduled = 0;

AudioStream   *AudioSystem::s_pFirstStream = 0;
AudioStream   *AudioSystem::s_pLastStream = 0;
audio_block_t *AudioSystem::s_pAudioMemory = 0;
audio_block_t *AudioSystem::s_pFreeBlock = 0;



void AudioSystem::start()
{
}


void AudioSystem::stop()
{
	// PRH TODO: stop and restart audioUpdateTask, objects in general
	s_bUpdateScheduled = false;
}



bool AudioSystem::initialize(u32 num_audio_blocks)
{
    LOG("initialize(%d)",num_audio_blocks);

    if (!initialize_memory(num_audio_blocks))
        return false;

    sortStreams();

    if (AudioCodec::s_pCodec)
    {
        LOG("Starting codec %s",AudioCodec::s_pCodec->getName());
        AudioCodec::s_pCodec->start();
    }

    for (AudioStream *p = s_pFirstStream; p; p = p->m_pNextStream)
    {
        LOG("Starting stream %s[%d]",p->getName(),p->getInstance());
        p->start();
    }

    return true;

}   // AudioSystem::initialize()



void AudioSystem::AddStream(AudioStream *pStream)
{
    if (s_pLastStream)
        s_pLastStream->m_pNextStream = pStream;
    else
        s_pFirstStream = pStream;
    s_pLastStream = pStream;
    s_numStreams++;    
}


void AudioSystem::resetStats()
{
	s_cpuCycles     = 0;
	s_cpuCyclesMax  = 0;
	s_numOverflows  = 0;
    
    for (AudioStream *p=s_pFirstStream; p; p=p->m_pNextStream)
        p->resetStats();
}


AudioStream *AudioSystem::find(u32 type, const char *name, s16 instance)
{
    for (AudioStream *p=s_pFirstStream; p; p=p->m_pNextStream)
    {
        bool type_match = (!type) || (type == p->getType());
        bool name_match = (!name) || (!strcmp(name,p->getName()));
        bool inst_match = (instance == -1) || (instance == (s16) p->getInstance());
        if (type_match && name_match && inst_match)
        {
            return p;            
        }
    }
    return NULL;
}


//----------------------------------------
// memory
//----------------------------------------


bool AudioSystem::initialize_memory(u32 num_audio_blocks)
{
    LOG("initialize_memory(%d)",num_audio_blocks);
    u32 bytes = num_audio_blocks * sizeof(audio_block_t);
    u32 avail = 0x40000000; // rsta2: skip mem_get_size check
    if (bytes > avail)
    {
        LOG_ERROR("cannot allocate %d memory blocks (%d bytes) max=%d bytes",
            num_audio_blocks,bytes,avail);
        return false;
    }
    
    s_pAudioMemory = (audio_block_t *) malloc(bytes);
    assert(s_pAudioMemory);
    if (!s_pAudioMemory)
    {
        LOG_ERROR("could not allocate %d memory blocks (%d bytes) max=%d bytes",
            num_audio_blocks,bytes,avail);
        return false;
    }

    // setup the free list
    
    s_totalBlocks = num_audio_blocks;
    audio_block_t *p = s_pAudioMemory;
    audio_block_t *prev = 0;
    
    for (u32 i=0; i<s_totalBlocks; i++)
    {
        p->next = 0;
        if (prev) prev->next = p;
        prev = p;
        p++;
    }
    
    s_pFreeBlock = s_pAudioMemory;
    return true;
}


// volatile bool show_allocs = 0;

audio_block_t *AudioSystem::allocate(void)
{
    // if (show_allocs)
    // {
    //     LOG("alloc   %d/%d  %08lx", s_blocksUsed,s_totalBlocks,(u32)s_pFreeBlock);
    //     delay(5);
    // }

	__disable_irq();

    if (!s_pFreeBlock)
    {
        static bool out_of_memory_error = 0;
        if (!out_of_memory_error)
            LOG_ERROR("OUT OF MEMORY",0);
        out_of_memory_error = 1;
    	__enable_irq();
        return NULL;
    }
    
    audio_block_t *block = s_pFreeBlock;
    s_pFreeBlock = block->next;
    s_pFreeBlock->prev = 0;

    block->prev = 0;
    block->next = 0;
   	block->ref_count = 1;

    s_blocksUsed++;
    if (s_blocksUsed > s_blocksUsedMax)
        s_blocksUsedMax = s_blocksUsed;
        
	__enable_irq();
    return block;
}	


void AudioSystem::release(audio_block_t *block)
{
    // assert(block);
		// prh 2025-03-10 was getting this assert at startup every time
		// so I removed it.
    if (!block)
        return;
    
	__disable_irq();
    
    if ( ((u32)block) < ((u32)s_pAudioMemory) ||
         ((u32)block) > ((u32)s_pAudioMemory) + ((u32)s_totalBlocks*AUDIO_BLOCK_BYTES) )
    {
        static bool bad_pointer_error = 0;
        if (!bad_pointer_error)
            LOG_ERROR("release BAD POINTER block(%08x) mem=(%08x to %08x)",
                (u32) block,
                (u32)s_pAudioMemory,
                ((u32)s_pAudioMemory) + ((u32)s_totalBlocks*AUDIO_BLOCK_BYTES));
        bad_pointer_error = 1;
    	__enable_irq();
        return;
    }
    
	if (block->ref_count > 1)
	{
		block->ref_count--;
	}
	else
    {    
        // if (show_allocs)
        // {
        //     LOG("release %d/%d  %08lx free=%08lx", s_blocksUsed,s_totalBlocks,(u32)block,(u32)s_pFreeBlock);
        //     delay(5);
        // }

        block->prev = 0;
        block->next = (audio_block_t *) s_pFreeBlock;
        if (s_pFreeBlock)
            s_pFreeBlock->prev = block;
        s_pFreeBlock = block;
        s_blocksUsed--;
    }
    
	__enable_irq();
}	


//----------------------------------------------
// update sorting
//----------------------------------------------


#define UPDATE_DEPTH_LIMIT  255
    // for detecting circularity
    // null nodes will be UPDATE_DEPTH_LIMIT+1
    // disconnected nodes will be UPDATE_DEPTH_LIMIT+2

    
void AudioSystem::traverse_update(u16 depth, AudioStream *p)
{
    if (!(p->getType() & AUDIO_DEVICE_INPUT))
        depth += 3;
    if (!(p->getType() & AUDIO_DEVICE_OUTPUT))
        depth += 3;
        
    if (depth >= UPDATE_DEPTH_LIMIT)
    {
        p->m_updateDepth = depth;				
        LOG_ERROR("update depth limit(%d) reached on %s%i (circular reference)",
            depth, p->getName(), p->getInstance());
        return;
    }
    if (depth > p->m_updateDepth)
        p->m_updateDepth = depth;
        
    #if 0
		CString fill;
        for (u16 i=0; i<depth; i++)
            fill.Append("  ");
		LOG("%s%s%d   depth:%d   max:%d",(const char *)fill,p->getName(),p->getInstance(),depth,p->m_updateDepth);
    #endif
    
    for (AudioConnection *con=p->m_pFirstConnection; con; con=con->m_pNextConnection)
    {
        traverse_update(depth+1,&con->m_dest);
    }
}


void AudioSystem::sortStreams()
    // Topologically sorting the updates is trivial if it is a tree.
    // If it is a graph (has cycles), it is not so easy.
    //
    // If TOPOLOGICAL_SORT is not defined, then you should declare
    // the AudioStream objects in the order you want the updates to occur,
    // i.e. declare inputs first, then effects, etc, then outputs.
    //
    // Our first approximation is a simple depth first tree traversal
    // with a limit to detect circularity. First we determine the "root"
    // input nodes and we traverse them, assigning the maximum depth of
    // each node visited.  NULL nodes (no inputs or outputs) can exists,
    // think of an i2sdevice that just outputs sound. It has no connections.
    // NULL nodes are placed after any visited nodes.
    //
    // Finally there may be dangling nodes, that have inputs and/or outputs
    // but, for some reason, were not visited during the traversal.  These
    // are just added last, then the list is sorted, then rebuilt.
{
    LOG("topologically sorting %d audio streams ...",s_numStreams);
    
    if (!s_numStreams)
    {
        LOG_ERROR("No AudioStreams found!!",0);
        return;
    }

	// initialize pUpdateDepth
    for (AudioStream *p = s_pFirstStream; p; p = p->m_pNextStream)
    {
		p->m_updateDepth = 0;
	}
	
	// traverse output only audio devices
    
    u16 obj_num = 0;
    AudioStream *objs[s_numStreams];
    for (AudioStream *p = s_pFirstStream; p; p = p->m_pNextStream)
    {
        objs[obj_num++] = p;
        if (!p->getNumInputs())
        {
            if (p->m_pFirstConnection)
            {
				// LOG("    traversing %s:%d start=%d",p->getName(),p->getInstance(),p->m_updateDepth);
                traverse_update(1,p);
            }
            else
            {
                LOG_WARNING("null update node: %s%d",p->getName(),p->getInstance());
                p->m_updateDepth = UPDATE_DEPTH_LIMIT + 1;
            }
        }
    }
    
    // mark dangling nodes
    
    for (u16 i=0; i<s_numStreams; i++)
    {
        AudioStream *p = objs[i];
        if (!p->m_updateDepth)
        {
            LOG_WARNING("dangling update node: %s%d",p->getName(),p->getInstance());
            p->m_updateDepth = UPDATE_DEPTH_LIMIT + 2;
        }
        p->m_pNextStream = 0;
    }
    
    // sort em
    
    u16 i = 0;
    while (i < (unsigned int) s_numStreams-1)
    {
        if (objs[i]->m_updateDepth > objs[i+1]->m_updateDepth)
        {
            AudioStream *tmp = objs[i];
            objs[i] = objs[i+1];
            objs[i+1] = tmp;
            if (i) i--;
        }
        else
        {
            i++;
        }
    }

    // rebuild the list

    s_pFirstStream = objs[0];
    AudioStream *prev = objs[0];
    for (i=1; i<s_numStreams; i++)
    {
		#if 0
			LOG("%d    %s:%d --> %s:%d",
				prev->m_updateDepth,
				prev->getName(),prev->getInstance(),
				objs[i]->getName(),objs[i]->getInstance());
		#endif
		
        prev->m_pNextStream = objs[i];
        prev = objs[i];
    }

    s_pLastStream = prev;
    // LOG("topo sort finished",0);
}


//----------------------------------------------
// update
//----------------------------------------------

bool AudioSystem::takeUpdateResponsibility()
{
	if (s_bUpdateScheduled)
		return false;
	s_bUpdateScheduled = true;
	return true;
    
}


// IN-ring-fill-driven drain. The earlier idempotent startUpdate scheduled at
// most ONE doUpdate per Core-1 wake; with the live engine in-line each
// doUpdate runs longer, so IN packets piled up faster than they drained —
// `avail` climbed to the 384 resync ceiling (in_rs) while the OUT ring
// emptied (out_av=0), chopping the audio into garbage WHENEVER engaged (even
// at unity scale, where the engine math is provably correct: effRate=1.000
// lock=1). Fix: when Core 1 wakes, doUpdate keeps running the graph while the
// IN ring still holds more than the target lag, draining it back to target in
// one wake instead of one-block-per-packet. This keeps both rings balanced
// under engine load without touching the ring files.
extern unsigned AudioInputUSB_inAvail (void);
#ifndef AUDIO_BLOCK_SAMPLES
#define AUDIO_BLOCK_SAMPLES 64
#endif
// IN ring resyncs if avail < AUDIO_BLOCK_SAMPLES (64) or >= 384 (3/4 of 512).
// The extra catch-up drain must pull avail DOWN from the overfill tendency
// (engine-engaged doUpdate runs slow -> ring fills toward 384) WITHOUT
// over-shooting to the underrun edge (which gave the periodic ~16s low-side
// resync = gurgle). Strategy: only drain an EXTRA block when avail is high
// enough that consuming one block still leaves >= DRAIN_FLOOR, and stop once
// avail is back near the ring's natural target. This brackets avail into a
// safe mid-band and lets the ring's own Q16 drift-correction hold the fine
// balance.
static const unsigned DRAIN_HIGH = AUDIO_BLOCK_SAMPLES * 4 + 32;  // ~288 enter-drain
static const unsigned DRAIN_LOW  = AUDIO_BLOCK_SAMPLES * 2;       // ~128 exit-drain
static const int      DRAIN_MAX_ITERS = 2;   // at most ONE extra block/wake —
                                             // multi-block drain bursts produced
                                             // 64-sample block-boundary clicks
static volatile bool s_updatePending = false;
static volatile bool s_draining = false;     // latched drain-mode (hysteresis)

void AudioSystem::startUpdate()
{
	// Mirror private counters to the DIAG globals (member has access).
	extern volatile unsigned g_diagNInUpdate, g_diagNumOverflows, g_diagUpdSched;
	g_diagNInUpdate = s_nInUpdate;
	g_diagNumOverflows = s_numOverflows;
	g_diagUpdSched = s_bUpdateScheduled ? 1u : 0u;
	// Atomic check+set: disable IRQ briefly to defeat ISR-vs-Core-2 race.
	__disable_irq();
	bool already = (s_nInUpdate != 0);
	if (already) {
		s_updatePending = true;   // catch up after the in-flight doUpdate
		__enable_irq();
		s_numOverflows++;
		return;
	}
	s_nInUpdate++;
	__enable_irq();

	#ifdef ARM_ALLOW_MULTI_CORE
		// Hand off DSP to Core 1 worker. Producer may be Core 0 ISR or
		// Core 2 watchdog. Push is allocation-free + DSB + SEV.
		coreDispatchPush(DISPATCH_AUDIO);
	#else
		doUpdate();
	#endif
}

// Capture-free diagnostics for the :4445 DIAG verb. The class statics are
// private, so startUpdate() (a member, has access) mirrors them into these
// file-scope globals each call; the free-function getters read the mirrors.
// nInUpdate stuck non-zero while no doUpdate completes == Core 1 not draining
// the dispatch ring (the graph-dead signature: cbwr/outWr frozen while IN fires).
volatile unsigned g_diagWalkN        = 0;
volatile unsigned g_diagTypeMask     = 0;
volatile unsigned g_diagNInUpdate    = 0;
volatile unsigned g_diagNumOverflows = 0;
volatile unsigned g_diagUpdSched     = 0;
unsigned AudioSystem_nInUpdate (void)     { return g_diagNInUpdate; }
unsigned AudioSystem_numOverflows (void)  { return g_diagNumOverflows; }
unsigned AudioSystem_updateScheduled (void) { return g_diagUpdSched; }


void AudioSystem::doUpdate()
{
	// Run the graph, then keep draining while the IN ring still holds more
	// than the target lag (one Core-1 wake catches up the whole backlog).
	// Capped so a sustained over-budget condition can't spin Core 1; the
	// ring resync clause remains the backstop.
	int guard = DRAIN_MAX_ITERS;
	for (;;)
	{
		// DIAG: count streams walked + OR together their getType() so the :4445
		// DIAG verb can prove whether the OUTPUT node (0x8000) is even in the
		// post-topo-sort s_pFirstStream chain this doUpdate traverses.
		extern volatile unsigned g_diagWalkN, g_diagTypeMask;
		unsigned walkN = 0, typeMask = 0;
		for (AudioStream *p = s_pFirstStream; p; p = p->m_pNextStream)
		{
			walkN++;
			typeMask |= p->getType();
			// An OUTPUT sink must drain its input queue every block to feed the
			// USB OUT ring, even though it has no OUTGOING connections. Gating
			// purely on m_numConnections skipped the AudioOutputUSB node (it read
			// 0 at the gate) so the OUT ring was never written -> silence
			// (witnessed: :4445 DIAG outUpd=0 while cbwr/looper.update climbed).
			if (p->m_numConnections || p->getType() == AUDIO_DEVICE_OUTPUT)
				p->update();
		}
		g_diagWalkN = walkN;
		g_diagTypeMask = typeMask;

		s_updatePending = false;
		// LATCHED HYSTERESIS drain. A bare threshold hunts: drain->over->stop->
		// refill->drain, oscillating every ~1s with a small resync each toggle.
		// Instead: only ENTER drain mode when avail climbs above DRAIN_HIGH
		// (near the 384 overfill edge), then drain all the way down to
		// DRAIN_LOW, and don't re-enter until avail climbs back above
		// DRAIN_HIGH. The wide [DRAIN_LOW, DRAIN_HIGH] deadband lets avail
		// sweep slowly between ~128 and ~288 — never near the 64 underrun or
		// 384 overfill resync edges, and no fast toggling.
		unsigned av = AudioInputUSB_inAvail();
		if (!s_draining && av > DRAIN_HIGH) s_draining = true;
		if (s_draining && av <= DRAIN_LOW)  s_draining = false;
		bool backlog = s_draining
		            && (av - AUDIO_BLOCK_SAMPLES >= DRAIN_LOW)
		            && (--guard > 0);
		if (!backlog)
		{
			__disable_irq();
			s_nInUpdate--;
			__enable_irq();
			return;
		}
	}
}

	
