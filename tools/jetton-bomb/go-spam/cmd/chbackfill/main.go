// chbackfill — one-off historical loader for a tpsmeter store.
//
// tpsmeter's own catch-up walks the chain block by block over a liteserver, which
// caps history at whatever the liteservers still keep (~5 days on testnet) and
// costs one RPC per block. The same blocks are already in toncenter's ClickHouse,
// so for a long history (a month) we stream them out in bulk here and commit them
// into the meter's bbolt store, then let the meter take over live from the head.
//
// The store's traversal invariants are respected exactly as the live backfill
// builds them: one step per masterchain block, descending and adjacent, each step
// carrying the blocks that masterchain block newly references plus the shard-top
// set it proves.
//
//	chbackfill --store hist.db --db testnet_d --days 30 \
//	  --ch http://tc-ch-02.toncenter.local:8123 --user vicread --password ...
//
// Afterwards run the meter on that same store (its own --catchup then completes
// immediately, since coverage already spans more than the largest window):
//
//	tpsmeter --store hist.db --listen 127.0.0.1:8092 -l config.json
package main

import (
	"bufio"
	"encoding/base64"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/tpsmeter"
)

const (
	mcWorkchain = int32(-1)
	mcShard     = int64(-9223372036854775808)
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "chbackfill:", err)
		os.Exit(1)
	}
}

type options struct {
	chURL    string
	user     string
	password string
	database string
	store    string
	days     float64
	headLag  int
	head     uint64
	chunk    uint64
	commit   int
}

func run() error {
	var opt options
	flag.StringVar(&opt.chURL, "ch", envOr("CH_URL", "http://tc-ch-02.toncenter.local:8123"), "ClickHouse HTTP endpoint")
	flag.StringVar(&opt.user, "user", envOr("CH_USER", "vicread"), "ClickHouse user")
	flag.StringVar(&opt.password, "password", os.Getenv("CH_PASSWORD"), "ClickHouse password")
	flag.StringVar(&opt.database, "db", envOr("CH_DB", "mainnet_d"), "ClickHouse database (mainnet_d | testnet_d)")
	flag.StringVar(&opt.store, "store", "", "tpsmeter bbolt store to fill (created if missing)")
	flag.Float64Var(&opt.days, "days", 30, "how far back to import")
	flag.IntVar(&opt.headLag, "head-lag", 120, "stop this many seconds short of now, so the head is fully indexed")
	flag.Uint64Var(&opt.head, "head-seqno", 0, "masterchain seqno to anchor at (default: newest indexed)")
	flag.Uint64Var(&opt.chunk, "chunk", 20000, "masterchain seqnos per ClickHouse query")
	flag.IntVar(&opt.commit, "commit", 2000, "masterchain steps per store transaction")
	flag.Parse()

	if opt.store == "" {
		return fmt.Errorf("--store is required")
	}
	if opt.password == "" {
		return fmt.Errorf("ClickHouse password is required (--password or CH_PASSWORD)")
	}
	if opt.days <= 0 || opt.chunk == 0 || opt.commit <= 0 {
		return fmt.Errorf("--days, --chunk and --commit must be positive")
	}

	ch := &client{base: opt.chURL, user: opt.user, password: opt.password, db: opt.database}

	head, headUtime, err := ch.head(opt.head, opt.headLag)
	if err != nil {
		return err
	}
	startUtime := headUtime - int64(opt.days*86400)
	low, err := ch.lowestMC(startUtime)
	if err != nil {
		return err
	}
	if low >= head {
		return fmt.Errorf("no masterchain range to import: low %d >= head %d", low, head)
	}
	fmt.Printf("# %s import mc [%d, %d] = %d blocks, %s .. %s\n", opt.database, low, head, head-low+1,
		time.Unix(startUtime, 0).UTC().Format(time.RFC3339), time.Unix(headUtime, 0).UTC().Format(time.RFC3339))

	store, err := tpsmeter.OpenStore(opt.store)
	if err != nil {
		return err
	}
	defer store.Close()

	loader := &loader{ch: ch, store: store, opt: opt}
	return loader.importRange(low, head)
}

// row is one ClickHouse blocks row.
type row struct {
	id       tpsmeter.BlockID
	mcSeqno  uint32
	utime    int64
	txCount  int
	identity tpsmeter.BlockIdentity
	tops     []tpsmeter.ShardID // masterchain rows only, seqno carried in topSeqnos
	topSeqno []uint32
}

func (r row) stat() tpsmeter.Stat {
	return tpsmeter.Stat{ID: r.id, GenUtime: r.utime, TxCount: r.txCount, IsMC: r.id.Workchain == mcWorkchain}
}

type loader struct {
	ch    *client
	store *tpsmeter.Store
	opt   options

	// blocks is the rolling row index used to resolve shard tops, which are
	// often older than the masterchain block that still references them;
	// byMC groups those rows by the masterchain block that first referenced them.
	blocks map[tpsmeter.BlockID]row
	byMC   map[uint32][]row
}

func (l *loader) index(rows []row) {
	l.blocks = make(map[tpsmeter.BlockID]row, len(rows)+1024)
	l.byMC = make(map[uint32][]row, len(rows)/2+16)
	for _, r := range rows {
		l.blocks[r.id] = r
		if r.id.Workchain != mcWorkchain {
			l.byMC[r.mcSeqno] = append(l.byMC[r.mcSeqno], r)
		}
	}
}

func (l *loader) importRange(low, head uint64) error {
	started := time.Now()

	// Anchor at the head: the masterchain block plus the shard tops it proves,
	// which frontierBounds needs already recorded.
	anchorRows, err := l.ch.rows(head, head)
	if err != nil {
		return err
	}
	l.index(anchorRows)
	anchorMC, ok := l.blocks[mcID(uint32(head))]
	if !ok {
		return fmt.Errorf("head masterchain block %d is missing from ClickHouse", head)
	}
	anchorStats, cp, err := l.step(anchorMC)
	if err != nil {
		return fmt.Errorf("anchor: %w", err)
	}
	if _, err := l.store.CommitForward(anchorStats, cp, anchorMC.utime, true); err != nil {
		return err
	}

	var (
		steps     []tpsmeter.BackfillStep
		committed uint64
		blocks    uint64
	)
	flush := func() error {
		if len(steps) == 0 {
			return nil
		}
		if _, err := l.store.CommitBackfillBatch(steps); err != nil {
			return err
		}
		committed += uint64(len(steps))
		steps = steps[:0]
		return nil
	}

	// Walk masterchain blocks backward in chunks. Each chunk is queried with a
	// margin below it, so a shard top that has not moved for a while is still in
	// the index when the step that references it is built.
	for hi := head - 1; hi >= low; {
		lo := low
		if hi >= low+l.opt.chunk {
			lo = hi - l.opt.chunk + 1
		}
		// Query below the chunk too: a shard top that has not moved for a while
		// is older than every masterchain block in the chunk that references it.
		margin := l.opt.chunk / 4
		queryLo := lo
		if queryLo > margin {
			queryLo -= margin
		} else {
			queryLo = 0
		}
		chunkRows, err := l.ch.rows(queryLo, hi)
		if err != nil {
			return err
		}
		l.index(chunkRows)
		for sq := hi; sq >= lo; sq-- {
			mc, ok := l.blocks[mcID(uint32(sq))]
			if !ok {
				return fmt.Errorf("masterchain block %d is missing from ClickHouse", sq)
			}
			stats, cp, err := l.step(mc)
			if err != nil {
				return err
			}
			blocks += uint64(len(stats))
			steps = append(steps, tpsmeter.BackfillStep{Stats: stats, Checkpoint: cp, MCUtime: mc.utime, Lineage: true})
			if len(steps) >= l.opt.commit {
				if err := flush(); err != nil {
					return err
				}
			}
			if sq == 0 {
				break
			}
		}
		if err := flush(); err != nil {
			return err
		}
		elapsed := time.Since(started).Seconds()
		fmt.Printf("\r# mc %d..%d done %d/%d (%.1f%%) blocks=%d %.0f mc/s   ",
			lo, hi, committed, head-low, 100*float64(committed)/float64(head-low), blocks, float64(committed)/elapsed)
		if lo == low {
			break
		}
		hi = lo - 1
	}
	fmt.Println()

	status, err := l.store.Status()
	if err != nil {
		return err
	}
	fmt.Printf("# store=%s epoch=%d blocks=%d coverage=%s..%s valid=%v in %s\n",
		l.store.Path(), status.Epoch, status.Blocks, fmtTime(status.CoverageStart), fmtTime(status.CoverageEnd),
		status.CoverageValid, time.Since(started).Truncate(time.Second))
	return nil
}

// step builds one masterchain step: the blocks it newly references, plus the
// shard tops it proves (already-stored ones are deduped away by the store).
func (l *loader) step(mc row) ([]tpsmeter.Stat, tpsmeter.Checkpoint, error) {
	stats := []tpsmeter.Stat{mc.stat()}
	shards := make(map[tpsmeter.ShardID]uint32, len(mc.tops))
	for i, shard := range mc.tops {
		seqno := mc.topSeqno[i]
		shards[shard] = seqno
		if seqno == 0 {
			continue
		}
		id := tpsmeter.BlockID{Workchain: shard.Workchain, Shard: shard.Shard, Seqno: seqno}
		top, ok := l.blocks[id]
		if !ok {
			var err error
			top, err = l.ch.block(id)
			if err != nil {
				return nil, tpsmeter.Checkpoint{}, fmt.Errorf("mc %d shard top %+v: %w", mc.id.Seqno, id, err)
			}
			l.blocks[id] = top
		}
		stats = append(stats, top.stat())
	}
	// Everything this masterchain block referenced for the first time.
	for _, r := range l.byMC[mc.id.Seqno] {
		stats = append(stats, r.stat())
	}
	cp := tpsmeter.Checkpoint{HaveMC: true, MCSeqno: mc.id.Seqno, MCIdentity: mc.identity, Shards: shards}
	return stats, cp, nil
}

func mcID(seqno uint32) tpsmeter.BlockID {
	return tpsmeter.BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: seqno}
}

func fmtTime(t *time.Time) string {
	if t == nil {
		return "-"
	}
	return t.UTC().Format(time.RFC3339)
}

// client is a minimal ClickHouse HTTP query client.
type client struct {
	base     string
	user     string
	password string
	db       string
	http     http.Client
}

const rowColumns = `workchain, shard, seqno, mc_block_seqno, gen_utime, transaction_count, root_hash, file_hash, ` +
	`arrayStringConcat(arrayMap(x -> concat(toString(x.1), ':', toString(x.2), ':', toString(x.3)), shards), ',')`

func (c *client) query(sql string) (io.ReadCloser, error) {
	endpoint := c.base + "/?" + url.Values{
		"user":     {c.user},
		"password": {c.password},
		"database": {c.db},
	}.Encode()
	response, err := c.http.Post(endpoint, "text/plain", strings.NewReader(sql))
	if err != nil {
		return nil, err
	}
	if response.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		response.Body.Close()
		return nil, fmt.Errorf("clickhouse HTTP %d: %s", response.StatusCode, strings.TrimSpace(string(body)))
	}
	return response.Body, nil
}

func (c *client) scalar(sql string) (string, error) {
	body, err := c.query(sql)
	if err != nil {
		return "", err
	}
	defer body.Close()
	raw, err := io.ReadAll(body)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(raw)), nil
}

func (c *client) head(requested uint64, lag int) (uint64, int64, error) {
	if requested != 0 {
		utime, err := c.scalar(fmt.Sprintf(
			"SELECT gen_utime FROM blocks WHERE workchain = -1 AND seqno = %d LIMIT 1 FORMAT TSV", requested))
		if err != nil {
			return 0, 0, err
		}
		if utime == "" {
			return 0, 0, fmt.Errorf("masterchain block %d is not in ClickHouse", requested)
		}
		parsed, err := strconv.ParseInt(utime, 10, 64)
		return requested, parsed, err
	}
	line, err := c.scalar(fmt.Sprintf(
		"SELECT seqno, gen_utime FROM blocks WHERE workchain = -1 AND gen_utime < now() - %d ORDER BY seqno DESC LIMIT 1 FORMAT TSV", lag))
	if err != nil {
		return 0, 0, err
	}
	fields := strings.Fields(line)
	if len(fields) != 2 {
		return 0, 0, fmt.Errorf("unexpected head row %q", line)
	}
	seqno, err := strconv.ParseUint(fields[0], 10, 32)
	if err != nil {
		return 0, 0, err
	}
	utime, err := strconv.ParseInt(fields[1], 10, 64)
	return seqno, utime, err
}

func (c *client) lowestMC(startUtime int64) (uint64, error) {
	line, err := c.scalar(fmt.Sprintf(
		"SELECT min(seqno) FROM blocks WHERE workchain = -1 AND gen_utime >= %d FORMAT TSV", startUtime))
	if err != nil {
		return 0, err
	}
	return strconv.ParseUint(strings.TrimSpace(line), 10, 32)
}

// rows returns every block first referenced by a masterchain block in [lo, hi],
// plus the masterchain blocks themselves.
func (c *client) rows(lo, hi uint64) ([]row, error) {
	body, err := c.query(fmt.Sprintf(
		"SELECT %s FROM blocks WHERE mc_block_seqno BETWEEN %d AND %d FORMAT TSV", rowColumns, lo, hi))
	if err != nil {
		return nil, err
	}
	defer body.Close()
	return scanRows(body)
}

func (c *client) block(id tpsmeter.BlockID) (row, error) {
	body, err := c.query(fmt.Sprintf(
		"SELECT %s FROM blocks WHERE workchain = %d AND shard = %d AND seqno = %d LIMIT 1 FORMAT TSV",
		rowColumns, id.Workchain, id.Shard, id.Seqno))
	if err != nil {
		return row{}, err
	}
	defer body.Close()
	rows, err := scanRows(body)
	if err != nil {
		return row{}, err
	}
	if len(rows) == 0 {
		return row{}, fmt.Errorf("block is not in ClickHouse")
	}
	return rows[0], nil
}

func scanRows(body io.Reader) ([]row, error) {
	scanner := bufio.NewScanner(body)
	scanner.Buffer(make([]byte, 0, 1<<20), 1<<22)
	var out []row
	for scanner.Scan() {
		parsed, err := parseRow(scanner.Text())
		if err != nil {
			return nil, err
		}
		out = append(out, parsed)
	}
	return out, scanner.Err()
}

func parseRow(line string) (row, error) {
	fields := strings.Split(line, "\t")
	if len(fields) != 9 {
		return row{}, fmt.Errorf("unexpected blocks row %q", line)
	}
	numbers := make([]int64, 6)
	for i := 0; i < 6; i++ {
		value, err := strconv.ParseInt(fields[i], 10, 64)
		if err != nil {
			return row{}, fmt.Errorf("blocks row %q: %w", line, err)
		}
		numbers[i] = value
	}
	identity, err := parseIdentity(fields[6], fields[7])
	if err != nil {
		return row{}, fmt.Errorf("blocks row %q: %w", line, err)
	}
	parsed := row{
		id:       tpsmeter.BlockID{Workchain: int32(numbers[0]), Shard: numbers[1], Seqno: uint32(numbers[2])},
		mcSeqno:  uint32(numbers[3]),
		utime:    numbers[4],
		txCount:  int(numbers[5]),
		identity: identity,
	}
	if parsed.id.Workchain != mcWorkchain {
		return parsed, nil
	}
	for _, top := range strings.Split(fields[8], ",") {
		if top == "" {
			continue
		}
		parts := strings.Split(top, ":")
		if len(parts) != 3 {
			return row{}, fmt.Errorf("unexpected shard top %q", top)
		}
		workchain, err := strconv.ParseInt(parts[0], 10, 32)
		if err != nil {
			return row{}, err
		}
		if int32(workchain) == mcWorkchain {
			continue
		}
		shard, err := strconv.ParseInt(parts[1], 10, 64)
		if err != nil {
			return row{}, err
		}
		seqno, err := strconv.ParseUint(parts[2], 10, 32)
		if err != nil {
			return row{}, err
		}
		parsed.tops = append(parsed.tops, tpsmeter.ShardID{Workchain: int32(workchain), Shard: shard})
		parsed.topSeqno = append(parsed.topSeqno, uint32(seqno))
	}
	return parsed, nil
}

func parseIdentity(rootHash, fileHash string) (tpsmeter.BlockIdentity, error) {
	var identity tpsmeter.BlockIdentity
	root, err := base64.StdEncoding.DecodeString(rootHash)
	if err != nil || len(root) != 32 {
		return identity, fmt.Errorf("invalid root hash %q", rootHash)
	}
	file, err := base64.StdEncoding.DecodeString(fileHash)
	if err != nil || len(file) != 32 {
		return identity, fmt.Errorf("invalid file hash %q", fileHash)
	}
	copy(identity.RootHash[:], root)
	copy(identity.FileHash[:], file)
	return identity, nil
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
