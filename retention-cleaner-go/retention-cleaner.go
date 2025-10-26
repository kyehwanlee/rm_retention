package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Config represents config.json structure.
type Config struct {
	Retention map[string]int `json:"retention"` // {"default":30, "1001":60, ...}
}

type options struct {
	root          string // data root, e.g. /data
	configPath    string // config.json path
	execute       bool   // actually delete (default false = dry run)
	workers       int    // concurrent delete workers
	deletesPerSec int    // throttle deletes per second (0 = unlimited)
	logEvery      int    // progress log interval
	targetCompany string // optional: restrict to a single company id
	nowUTC        time.Time
}

func main() {
	var opt options
	flag.StringVar(&opt.root, "root", "/data", "Data root directory, e.g. /data")
	flag.StringVar(&opt.configPath, "config", "./config.json", "Path to config.json")
	flag.BoolVar(&opt.execute, "execute", false, "Perform deletions (default: dry-run)")
	flag.IntVar(&opt.workers, "workers", minInt(4, runtime.NumCPU()), "Concurrent delete workers")
	flag.IntVar(&opt.deletesPerSec, "deletes-per-sec", 20, "Max deletions per second (0 = unlimited)")
	flag.IntVar(&opt.logEvery, "log-every", 1000, "Log progress every N minute-dirs scanned")
	flag.StringVar(&opt.targetCompany, "company", "", "If set, only process this company id")
	flag.Parse()

	opt.nowUTC = time.Now().UTC()

	cfg, err := loadConfig(opt.configPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	if _, ok := cfg.Retention["default"]; !ok {
		log.Fatalf("config.json missing retention.default")
	}

	log.Printf("starting retention cleaner (dry-run=%v) root=%s workers=%d rps=%d now=%s",
		!opt.execute, opt.root, opt.workers, opt.deletesPerSec, opt.nowUTC.Format(time.RFC3339))

	ctx := context.Background()

	var scanned, candidates, deleted, failed uint64

	// deletion jobs channel
	jobs := make(chan string, 1024)

	// throttle (token bucket by time.Ticker)
	var throttle <-chan time.Time
	if opt.deletesPerSec > 0 {
		ticker := time.NewTicker(time.Second / time.Duration(opt.deletesPerSec))
		defer ticker.Stop()
		throttle = ticker.C
	}

	// worker pool
	var wg sync.WaitGroup
	for i := 0; i < opt.workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for path := range jobs {
				if throttle != nil {
					select {
					case <-ctx.Done():
						return
					case <-throttle:
					}
				}
				if !opt.execute {
					// dry-run: pretend success
					atomic.AddUint64(&deleted, 1)
					continue
				}
				// actual deletion
				if err := os.RemoveAll(path); err != nil {
					log.Printf("[WARN] delete failed: %s: %v", path, err)
					atomic.AddUint64(&failed, 1)
					continue
				}
				atomic.AddUint64(&deleted, 1)
			}
		}()
	}

	// Traverse /data
	err = walkCompanies(opt, cfg, func(minuteDir string) {
		atomic.AddUint64(&candidates, 1)
		jobs <- minuteDir
	}, func() {
		n := atomic.AddUint64(&scanned, 1)
		if opt.logEvery > 0 && (n%uint64(opt.logEvery) == 0) {
			log.Printf("progress: scanned=%d candidates=%d deleted(dry-count)=%d failed=%d",
				atomic.LoadUint64(&scanned), atomic.LoadUint64(&candidates),
				atomic.LoadUint64(&deleted), atomic.LoadUint64(&failed))
		}
	})
	if err != nil {
		log.Fatalf("walk error: %v", err)
	}

	close(jobs)
	wg.Wait()

	log.Printf("done. scanned=%d candidates=%d deleted=%d failed=%d (dry-run=%v)",
		scanned, candidates, deleted, failed, !opt.execute)
}

func loadConfig(p string) (*Config, error) {
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var cfg Config
	if err := json.Unmarshal(b, &cfg); err != nil {
		return nil, err
	}
	if cfg.Retention == nil {
		return nil, errors.New("retention map missing")
	}
	return &cfg, nil
}

// walkCompanies walks the tree and sends paths of minute directories older than retention.
func walkCompanies(opt options, cfg *Config, onCandidate func(minuteDir string), onProgress func()) error {
	// If company filter provided, only traverse that sub-tree.
	if opt.targetCompany != "" {
		companyPath := filepath.Join(opt.root, opt.targetCompany)
		return walkCompany(opt, cfg, opt.targetCompany, companyPath, onCandidate, onProgress)
	}

	entries, err := os.ReadDir(opt.root)
	if err != nil {
		return fmt.Errorf("read root: %w", err)
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		companyID := e.Name()
		companyPath := filepath.Join(opt.root, companyID)
		if err := walkCompany(opt, cfg, companyID, companyPath, onCandidate, onProgress); err != nil {
			// continue on error, but log
			log.Printf("[WARN] walk company %s: %v", companyID, err)
		}
	}
	return nil
}

func walkCompany(opt options, cfg *Config, companyID, companyPath string, onCandidate func(string), onProgress func()) error {
	retentionDays := retentionFor(companyID, cfg)
	cutoff := opt.nowUTC.AddDate(0, 0, -retentionDays)

	// Depth-aware walk to minimize stats:
	// /company/device/year/month/day/hour/minute
	// We only dive into directories, ignore files early.
	return walkDirOnly(companyPath, 1, func(path string, relParts []string) error {
		// relParts includes: [company, device, year, month, day, hour, min]
		switch len(relParts) {
		case 1:
			// company level -> go deeper to device
			return nil
		case 2:
			// device level
			return nil
		case 3, 4, 5, 6:
			// year/month/day/hour -> continue
			return nil
		case 7:
			// minute directory: evaluate timestamp
			ts, ok := parseTimestampFromParts(relParts[2], relParts[3], relParts[4], relParts[5], relParts[6])
			if !ok {
				// malformed name, skip
				return nil
			}
			if ts.Before(cutoff) {
				onCandidate(path)
			}
			onProgress()
			// do not recurse below minute dir
			return fs.SkipDir
		default:
			// deeper than expected; skip
			return fs.SkipDir
		}
	})
}

func parseTimestampFromParts(year, month, day, hour, minute string) (time.Time, bool) {
	y, err1 := strconv.Atoi(year)
	m, err2 := strconv.Atoi(month)
	d, err3 := strconv.Atoi(day)
	h, err4 := strconv.Atoi(hour)
	min, err5 := strconv.Atoi(minute)
	if err1 != nil || err2 != nil || err3 != nil || err4 != nil || err5 != nil {
		return time.Time{}, false
	}
	// UTC per problem statement
	return time.Date(y, time.Month(m), d, h, min, 0, 0, time.UTC), true
}

func retentionFor(companyID string, cfg *Config) int {
	if days, ok := cfg.Retention[companyID]; ok {
		return days
	}
	return cfg.Retention["default"]
}

// walkDirOnly walks directories under base, calling cb for each directory.
// It computes relative parts to control depth and prune files early.
func walkDirOnly(base string, minDepth int, cb func(path string, relParts []string) error) error {
	base = filepath.Clean(base)

	return filepath.WalkDir(base, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			// Permission or transient errors: log and skip subtree
			log.Printf("[WARN] walk: %s: %v", path, err)
			if d != nil && d.IsDir() {
				return fs.SkipDir
			}
			return nil
		}
		if !d.IsDir() {
			// skip files to reduce IO
			return nil
		}
		rel, err := filepath.Rel(filepath.Dir(base), path)
		if err != nil {
			return nil
		}
		parts := splitClean(rel)
		// parts[0] == company at company root level
		// Ensure we're not acting above expected base
		if len(parts) < minDepth {
			return nil
		}
		return cb(path, parts)
	})
}

func splitClean(p string) []string {
	if p == "" || p == "." {
		return nil
	}
	parts := strings.Split(p, string(os.PathSeparator))
	out := make([]string, 0, len(parts))
	for _, s := range parts {
		s = strings.TrimSpace(s)
		if s == "" || s == "." {
			continue
		}
		out = append(out, s)
	}
	return out
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
