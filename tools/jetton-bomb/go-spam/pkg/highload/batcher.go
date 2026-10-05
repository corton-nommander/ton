package highload

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

var ErrBatchQueueFull = errors.New("highload: batch queue is full")

// Limiter is shared across runs so stopping/restarting a run cannot reset the per-address pacing.
// It limits ALL RPC attempts, including retries, to one start every 500ms.
type Limiter struct {
	mu   sync.Mutex
	next map[string]time.Time
}

func (l *Limiter) wait(ctx context.Context, addr *address.Address) error {
	key := addr.StringRaw()
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		l.mu.Lock()
		now := time.Now()
		next := l.next[key]
		if !now.Before(next) {
			if l.next == nil {
				l.next = make(map[string]time.Time)
			}
			l.next[key] = now.Add(BatchInterval)
			l.mu.Unlock()
			return nil
		}
		l.mu.Unlock()
		timer := time.NewTimer(time.Until(next))
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
}

type BatchOptions struct {
	SendTimeout func() time.Duration
	NoRetry     func() bool
	Limiter     *Limiter
	OnAttempt   func(retry bool, requests int)
	OnAccepted  func(requests int)
	// OnDispatch receives the immutable external and its constituent request
	// contexts immediately before the first RPC attempt. A batch cancelled while
	// waiting for the limiter never becomes a delivery-monitor sample.
	OnDispatch func(dst *address.Address, external *cell.Cell, requests []context.Context)
}

type BatchSendFunc func(context.Context, *address.Address, *cell.Cell) error

type batchRequest struct {
	ctx    context.Context
	target *walletspam.WalletSpam
	seqno  uint32
	value  tlb.Coins
	action *cell.Cell
	result chan error
	state  atomic.Int32 // 0 queued, 1 cancelled before dispatch, 2 committed to immutable external
}

type batchWorker struct {
	wallet *HighloadWallet
	input  chan *batchRequest
}

// Batcher is the sole production gasless submission path. Submit waits for the containing batch's
// RPC result; acceptance is not a claim that every downstream sint has executed.
type Batcher struct {
	ctx     context.Context
	cancel  context.CancelFunc
	workers []batchWorker
	send    BatchSendFunc
	options BatchOptions
	wg      sync.WaitGroup
}

func NewBatcher(ctx context.Context, set []*HighloadWallet, send BatchSendFunc, options BatchOptions) *Batcher {
	if len(set) != NumShards && len(set) != MaxShards {
		panic("highload: batcher requires 4 or 16 relayers")
	}
	ctx, cancel := context.WithCancel(ctx)
	if options.Limiter == nil {
		options.Limiter = &Limiter{}
	}
	b := &Batcher{ctx: ctx, cancel: cancel, send: send, options: options}
	b.workers = make([]batchWorker, len(set))
	firstWindow := time.Now().Add(BatchInterval)
	for i, w := range set {
		b.workers[i] = batchWorker{wallet: w, input: make(chan *batchRequest, BatchQueueLimit)}
		b.wg.Add(1)
		go b.run(&b.workers[i], firstWindow)
	}
	return b
}

func (b *Batcher) Close() { b.cancel(); b.wg.Wait() }

// An RPC timeout or error after dispatch must not free the target's pending seqno: the external may
// already be in the network. pool.Run recognizes this interface without importing highload.
type submissionError struct{ err error }

func (e *submissionError) Error() string         { return e.err.Error() }
func (e *submissionError) Unwrap() error         { return e.err }
func (e *submissionError) MayHaveBeenSent() bool { return true }

func (b *Batcher) Submit(ctx context.Context, target *walletspam.WalletSpam, seqno uint32, value tlb.Coins, action *cell.Cell) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := b.ctx.Err(); err != nil {
		return err
	}
	r := &batchRequest{ctx: ctx, target: target, seqno: seqno, value: value, action: action, result: make(chan error, 1)}
	select {
	case b.workers[ShardFor(target.Address(), len(b.workers))].input <- r:
	case <-ctx.Done():
		return ctx.Err()
	case <-b.ctx.Done():
		return b.ctx.Err()
	}
	select {
	case err := <-r.result:
		return err
	case <-ctx.Done():
		if r.state.CompareAndSwap(0, 1) {
			return ctx.Err()
		}
		return &submissionError{ctx.Err()}
	case <-b.ctx.Done():
		if r.state.CompareAndSwap(0, 1) {
			return b.ctx.Err()
		}
		return &submissionError{b.ctx.Err()}
	}
}

func finish(r *batchRequest, err error) {
	select {
	case r.result <- err:
	default:
	}
}

func (b *Batcher) run(w *batchWorker, firstWindow time.Time) {
	defer b.wg.Done()
	queue := make([]*batchRequest, 0, BatchQueueLimit)
	timer := time.NewTimer(time.Until(firstWindow))
	defer timer.Stop()
	nextWindow := firstWindow
	defer func() {
		for _, r := range queue {
			finish(r, b.ctx.Err())
		}
		for {
			select {
			case r := <-w.input:
				finish(r, b.ctx.Err())
			default:
				return
			}
		}
	}()
	for {
		select {
		case <-b.ctx.Done():
			return
		case r := <-w.input:
			if len(queue) >= BatchQueueLimit {
				finish(r, ErrBatchQueueFull)
			} else {
				queue = append(queue, r)
			}
		case <-timer.C:
			nextWindow = nextWindow.Add(BatchInterval)
			// Include requests that reached the channel just before this window closed.
			drain := true
			for drain && len(queue) < BatchQueueLimit {
				select {
				case r := <-w.input:
					queue = append(queue, r)
				default:
					drain = false
				}
			}
			queue = b.flush(w, queue)
			// No catch-up bursts after a slow RPC. The shared limiter also paces retry attempts.
			for !nextWindow.After(time.Now()) {
				nextWindow = nextWindow.Add(BatchInterval)
			}
			timer.Reset(time.Until(nextWindow))
		}
	}
}

func (b *Batcher) flush(w *batchWorker, queue []*batchRequest) []*batchRequest {
	live := queue[:0]
	for _, r := range queue {
		if r.state.Load() == 1 || r.ctx.Err() != nil {
			r.state.CompareAndSwap(0, 1)
			finish(r, r.ctx.Err())
		} else {
			live = append(live, r)
		}
	}
	queue = live
	if len(queue) == 0 {
		return queue
	}
	n := len(queue)
	if n > MaxBatchMessages {
		n = MaxBatchMessages
	}
	candidates := queue[:n]
	rest := queue[n:]
	requests := make([]*batchRequest, 0, n)
	messages := make([]*cell.Cell, 0, n)
	validUntil := uint32(time.Now().Add(BatchValidity).Unix())
	for _, r := range candidates {
		msg, err := BuildRelayMessage(r.target, r.seqno, validUntil, r.value, r.action)
		if err != nil {
			finish(r, err)
			continue
		}
		requests = append(requests, r)
		messages = append(messages, msg)
	}
	if len(messages) == 0 {
		return rest
	}
	fit := len(messages)
	if size, err := w.wallet.batchSize(messages); err != nil || size > MaxBatchBytes {
		lo, hi := 0, len(messages)
		for lo < hi {
			mid := (lo + hi + 1) / 2
			size, err := w.wallet.batchSize(messages[:mid])
			if err == nil && size <= MaxBatchBytes {
				lo = mid
			} else {
				hi = mid - 1
			}
		}
		fit = lo
	}
	if fit == 0 {
		finish(requests[0], fmt.Errorf("highload: relay does not fit the %d-byte batch limit", MaxBatchBytes))
		return append(requests[1:], rest...)
	}
	rest = append(append([]*batchRequest{}, requests[fit:]...), rest...)
	committed := requests[:0]
	ready := messages[:0]
	for i, r := range requests[:fit] {
		if r.ctx.Err() == nil && r.state.CompareAndSwap(0, 2) {
			committed = append(committed, r)
			ready = append(ready, messages[i])
		} else {
			r.state.CompareAndSwap(0, 1)
			finish(r, r.ctx.Err())
		}
	}
	if len(ready) == 0 {
		return rest
	}
	ext, err := w.wallet.BuildBatchExternal(ready, CreatedAtNow())
	if err == nil {
		for attempt := 0; attempt < 2; attempt++ {
			if err = b.options.Limiter.wait(b.ctx, w.wallet.Address()); err != nil {
				break
			}
			timeout := 5 * time.Second
			if b.options.SendTimeout != nil {
				if t := b.options.SendTimeout(); t > 0 {
					timeout = t
				}
			}
			ctx, cancel := context.WithTimeout(b.ctx, timeout)
			if attempt == 0 && b.options.OnDispatch != nil {
				contexts := make([]context.Context, 0, len(committed))
				for _, r := range committed {
					contexts = append(contexts, r.ctx)
				}
				b.options.OnDispatch(w.wallet.Address(), ext, contexts)
			}
			if b.options.OnAttempt != nil {
				b.options.OnAttempt(attempt > 0, len(ready))
			}
			err = b.send(ctx, w.wallet.Address(), ext)
			cancel()
			if err == nil {
				if b.options.OnAccepted != nil {
					b.options.OnAccepted(len(ready))
				}
				break
			}
			if b.options.NoRetry != nil && b.options.NoRetry() {
				break
			}
		}
	}
	if err != nil {
		err = &submissionError{err}
	}
	for _, r := range committed {
		finish(r, err)
	}
	return rest
}
