use crate::resp;
use crate::skiplist::Skiplist;
use std::collections::{HashMap, VecDeque};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Clone)]
enum Value {
    Str(Vec<u8>),
    List(VecDeque<Vec<u8>>),
    Hash(HashMap<Vec<u8>, Vec<u8>>),
    Set(HashMap<Vec<u8>, ()>),
    Zset { order: Skiplist, scores: HashMap<Vec<u8>, f64> },
}

struct Entry {
    value: Value,
    expire: Option<i64>,
}

pub(crate) struct Db {
    keys: HashMap<Vec<u8>, Entry>,
    touch: HashMap<Vec<u8>, u64>,
}

pub struct Session {
    pub id: u64,
    pub db: usize,
    pub proto: i32,
    pub authed: bool,
    pub reply: Vec<u8>,
    pub name: Option<Vec<u8>>,
    multi: bool,
    queue: Vec<Vec<Vec<u8>>>,
    dirty_exec: bool,
    dirty_cas: bool,
    watched: HashMap<Vec<u8>, u64>,
    subs: Vec<Vec<u8>>,
    pubsub: bool,
    skip_aof: bool,
}

pub struct Hub {
    pub(crate) dbs: Vec<Db>,
    pub sessions: HashMap<u64, Session>,
    channels: HashMap<Vec<u8>, Vec<u64>>,
    pub appendonly: bool,
    pub loading: bool,
    pub dir: PathBuf,
    pub aof_name: String,
    pub db_name: String,
    pub requirepass: Vec<u8>,
    pub maxmemory: u64,
    pub timeout_secs: u64,
    pub port: u16,
    pub next_id: u64,
    pub commands: i64,
    pub shutdown: bool,
    pub force_save: bool,
    pub no_save: bool,
    aof: Option<File>,
    aof_db: i32,
    used: u64,
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn eq(arg: &[u8], lit: &str) -> bool {
    arg.len() == lit.len()
        && arg
            .iter()
            .zip(lit.bytes())
            .all(|(a, b)| a.to_ascii_lowercase() == b)
}

fn glob(pat: &[u8], text: &[u8]) -> bool {
    fn rec(pat: &[u8], text: &[u8]) -> bool {
        let mut pi = 0;
        let mut ti = 0;
        while pi < pat.len() && ti < text.len() {
            if pat[pi] == b'*' {
                while pi < pat.len() && pat[pi] == b'*' {
                    pi += 1;
                }
                if pi == pat.len() {
                    return true;
                }
                while ti <= text.len() {
                    if rec(&pat[pi..], &text[ti..]) {
                        return true;
                    }
                    ti += 1;
                }
                return false;
            }
            if pat[pi] == b'?' || pat[pi] == text[ti] {
                pi += 1;
                ti += 1;
                continue;
            }
            return false;
        }
        while pi < pat.len() && pat[pi] == b'*' {
            pi += 1;
        }
        pi == pat.len() && ti == text.len()
    }
    rec(pat, text)
}

fn parse_i64(bytes: &[u8]) -> Option<i64> {
    if bytes.is_empty() {
        return None;
    }
    let (neg, rest) = match bytes[0] {
        b'+' => (false, &bytes[1..]),
        b'-' => (true, &bytes[1..]),
        _ => (false, bytes),
    };
    if rest.is_empty() || (rest[0] == b'0' && rest.len() != 1) {
        return None;
    }
    let mut acc: u64 = 0;
    for byte in rest {
        if !byte.is_ascii_digit() {
            return None;
        }
        acc = acc.checked_mul(10)?.checked_add(u64::from(byte - b'0'))?;
        if !neg && acc > i64::MAX as u64 {
            return None;
        }
        if neg && acc > i64::MAX as u64 + 1 {
            return None;
        }
    }
    if neg && acc == i64::MAX as u64 + 1 {
        Some(i64::MIN)
    } else if neg {
        Some(-(acc as i64))
    } else {
        Some(acc as i64)
    }
}

fn parse_f64(bytes: &[u8]) -> Option<f64> {
    let text = std::str::from_utf8(bytes).ok()?;
    let n: f64 = text.parse().ok()?;
    if text.len() != bytes.len() {
        return None;
    }
    Some(n)
}

impl Session {
    fn new(id: u64, authed: bool) -> Self {
        Self {
            id,
            db: 0,
            proto: 2,
            authed,
            reply: Vec::new(),
            name: None,
            multi: false,
            queue: Vec::new(),
            dirty_exec: false,
            dirty_cas: false,
            watched: HashMap::new(),
            subs: Vec::new(),
            pubsub: false,
            skip_aof: false,
        }
    }
    fn simple(&mut self, text: &str) {
        self.reply.extend(resp::simple(text));
    }
    fn err(&mut self, text: &str) {
        self.reply.extend(resp::error(text));
    }
    fn int(&mut self, n: i64) {
        self.reply.extend(resp::integer(n));
    }
    fn bulk(&mut self, data: &[u8]) {
        self.reply.extend(resp::bulk(data));
    }
    fn null(&mut self) {
        self.reply.extend(resp::null(self.proto));
    }
    fn null_array(&mut self) {
        self.reply.extend(resp::null_array(self.proto));
    }
    fn arr(&mut self, n: i64) {
        self.reply.extend(resp::array_len(n));
    }
    fn map(&mut self, n: i64) {
        self.reply.extend(resp::map_len(self.proto, n));
    }
    fn push(&mut self, n: i64) {
        self.reply.extend(resp::push_len(self.proto, n));
    }
}

impl Hub {
    pub fn new(databases: usize) -> Self {
        let mut dbs = Vec::new();
        for _ in 0..databases.max(1) {
            dbs.push(Db {
                keys: HashMap::new(),
                touch: HashMap::new(),
            });
        }
        Self {
            dbs,
            sessions: HashMap::new(),
            channels: HashMap::new(),
            appendonly: false,
            loading: false,
            dir: PathBuf::from("."),
            aof_name: "appendonly.aof".into(),
            db_name: "dump.srdb".into(),
            requirepass: Vec::new(),
            maxmemory: 0,
            timeout_secs: 0,
            port: 6379,
            next_id: 1,
            commands: 0,
            shutdown: false,
            force_save: false,
            no_save: false,
            aof: None,
            aof_db: -1,
            used: 0,
        }
    }

    pub fn add_session(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        let authed = self.requirepass.is_empty();
        self.sessions.insert(id, Session::new(id, authed));
        id
    }

    pub fn take_reply(&mut self, id: u64) -> Vec<u8> {
        let session = self.sessions.get_mut(&id).unwrap();
        std::mem::take(&mut session.reply)
    }

    fn path(&self, name: &str) -> PathBuf {
        self.dir.join(name)
    }

    pub fn open_aof(&mut self) {
        if !self.appendonly {
            return;
        }
        let path = self.path(&self.aof_name);
        if let Ok(file) = OpenOptions::new().create(true).read(true).write(true).open(&path) {
            self.aof = Some(file);
        }
    }

    pub fn load_aof(&mut self) {
        let Some(file) = self.aof.as_mut() else {
            return;
        };
        let _ = file.seek(SeekFrom::Start(0));
        let mut buf = Vec::new();
        let _ = file.read_to_end(&mut buf);
        let _ = file.seek(SeekFrom::End(0));
        self.loading = true;
        let id = self.add_session();
        let mut off = 0;
        while off < buf.len() {
            match resp::parse(&buf[off..]) {
                resp::Parse::Done { value, consumed } => {
                    if let resp::Value::Array(items) = value {
                        let args: Vec<Vec<u8>> = items
                            .into_iter()
                            .map(|v| match v {
                                resp::Value::Bulk(b) => b,
                                _ => Vec::new(),
                            })
                            .collect();
                        self.exec(id, args);
                        let _ = self.take_reply(id);
                    }
                    off += consumed;
                }
                _ => break,
            }
        }
        self.sessions.remove(&id);
        self.loading = false;
    }

    fn feed(&mut self, db: usize, args: &[Vec<u8>]) {
        if !self.appendonly || self.loading {
            return;
        }
        let Some(file) = self.aof.as_mut() else {
            return;
        };
        if self.aof_db != db as i32 {
            let select = vec![b"SELECT".to_vec(), db.to_string().into_bytes()];
            write_argv(file, &select);
            self.aof_db = db as i32;
            let _ = select;
        }
        write_argv(file, args);
    }

    fn touch(db: &mut Db, key: &[u8]) {
        *db.touch.entry(key.to_vec()).or_insert(0) += 1;
    }

    fn expired(entry: &Entry) -> bool {
        entry.expire.is_some_and(|when| when <= now_ms())
    }

    fn lookup<'a>(db: &'a mut Db, key: &[u8]) -> Option<&'a mut Entry> {
        if db.keys.get(key).is_some_and(Self::expired) {
            if let Some(entry) = db.keys.remove(key) {
                let _ = entry;
            }
            Self::touch(db, key);
            return None;
        }
        db.keys.get_mut(key)
    }

    fn type_name(value: &Value) -> &'static str {
        match value {
            Value::Str(_) => "string",
            Value::List(_) => "list",
            Value::Hash(_) => "hash",
            Value::Set(_) => "set",
            Value::Zset { .. } => "zset",
        }
    }

    pub fn exec(&mut self, id: u64, args: Vec<Vec<u8>>) {
        if args.is_empty() {
            return;
        }
        self.commands += 1;
        let name = args[0].clone();
        let session = self.sessions.get(&id).unwrap();
        if !session.authed && !matches_any(&name, &["auth", "ping", "hello", "quit"]) {
            self.sessions.get_mut(&id).unwrap().err("NOAUTH Authentication required.");
            return;
        }
        if session.pubsub && !matches_any(&name, &["subscribe", "unsubscribe", "ping", "quit"]) {
            let text = format!(
                "ERR Can't execute '{}': only SUBSCRIBE / UNSUBSCRIBE / PING / QUIT are allowed in this context",
                String::from_utf8_lossy(&name).to_ascii_lowercase()
            );
            self.sessions.get_mut(&id).unwrap().err(&text);
            return;
        }
        let known = is_known(&name);
        if !known {
            let text = format!(
                "ERR unknown command '{}'",
                String::from_utf8_lossy(&name)
            );
            let session = self.sessions.get_mut(&id).unwrap();
            session.err(&text);
            if session.multi {
                session.dirty_exec = true;
            }
            return;
        }
        if session.multi && !matches_any(&name, &["exec", "discard", "multi", "watch", "unwatch"]) {
            let session = self.sessions.get_mut(&id).unwrap();
            if bad_syntax(&name, &args) {
                session.dirty_exec = true;
                syntax_error(session, &name, &args);
                return;
            }
            session.queue.push(args);
            session.simple("QUEUED");
            return;
        }
        if bad_syntax(&name, &args) {
            let session = self.sessions.get_mut(&id).unwrap();
            if session.multi {
                session.dirty_exec = true;
            }
            syntax_error(session, &name, &args);
            return;
        }
        self.dispatch(id, args);
    }

    fn dispatch(&mut self, id: u64, args: Vec<Vec<u8>>) {
        let name = args[0].to_ascii_lowercase();
        match name.as_slice() {
            b"ping" => self.cmd_ping(id, &args),
            b"echo" => {
                let session = self.sessions.get_mut(&id).unwrap();
                session.bulk(&args[1]);
            }
            b"quit" => {
                let session = self.sessions.get_mut(&id).unwrap();
                session.simple("OK");
            }
            b"select" => self.cmd_select(id, &args),
            b"get" => self.cmd_get(id, &args),
            b"set" => self.cmd_set(id, &args),
            b"setnx" => self.cmd_setnx(id, &args),
            b"setex" => self.cmd_setex(id, &args),
            b"mget" => self.cmd_mget(id, &args),
            b"mset" => self.cmd_mset(id, &args),
            b"incr" => self.cmd_incr(id, &args, 1),
            b"decr" => self.cmd_incr(id, &args, -1),
            b"incrby" => self.cmd_incrby(id, &args, false),
            b"decrby" => self.cmd_incrby(id, &args, true),
            b"append" => self.cmd_append(id, &args),
            b"strlen" => self.cmd_strlen(id, &args),
            b"del" => self.cmd_del(id, &args),
            b"exists" => self.cmd_exists(id, &args),
            b"type" => self.cmd_type(id, &args),
            b"keys" => self.cmd_keys(id, &args),
            b"scan" => self.cmd_scan(id, &args),
            b"expire" | b"pexpire" | b"expireat" | b"pexpireat" => self.cmd_expire(id, &args),
            b"ttl" => self.cmd_ttl(id, &args, false),
            b"pttl" => self.cmd_ttl(id, &args, true),
            b"persist" => self.cmd_persist(id, &args),
            b"dbsize" => self.cmd_dbsize(id),
            b"flushdb" => self.cmd_flush(id, false),
            b"flushall" => self.cmd_flush(id, true),
            b"lpush" => self.cmd_push(id, &args, true),
            b"rpush" => self.cmd_push(id, &args, false),
            b"lpop" => self.cmd_pop(id, &args, true),
            b"rpop" => self.cmd_pop(id, &args, false),
            b"llen" => self.cmd_llen(id, &args),
            b"lindex" => self.cmd_lindex(id, &args),
            b"lrange" => self.cmd_lrange(id, &args),
            b"hset" => self.cmd_hset(id, &args),
            b"hget" => self.cmd_hget(id, &args),
            b"hdel" => self.cmd_hdel(id, &args),
            b"hexists" => self.cmd_hexists(id, &args),
            b"hlen" => self.cmd_hlen(id, &args),
            b"hgetall" => self.cmd_hgetall(id, &args),
            b"hkeys" => self.cmd_hkeys(id, &args, true),
            b"hvals" => self.cmd_hkeys(id, &args, false),
            b"sadd" => self.cmd_sadd(id, &args),
            b"srem" => self.cmd_srem(id, &args),
            b"sismember" => self.cmd_sismember(id, &args),
            b"smembers" => self.cmd_smembers(id, &args),
            b"scard" => self.cmd_scard(id, &args),
            b"zadd" => self.cmd_zadd(id, &args),
            b"zrem" => self.cmd_zrem(id, &args),
            b"zscore" => self.cmd_zscore(id, &args),
            b"zcard" => self.cmd_zcard(id, &args),
            b"zrange" => self.cmd_zrange(id, &args),
            b"zrank" => self.cmd_zrank(id, &args),
            b"multi" => self.cmd_multi(id),
            b"exec" => self.cmd_exec(id),
            b"discard" => self.cmd_discard(id),
            b"watch" => self.cmd_watch(id, &args),
            b"unwatch" => self.cmd_unwatch(id),
            b"subscribe" => self.cmd_subscribe(id, &args),
            b"unsubscribe" => self.cmd_unsubscribe(id, &args),
            b"publish" => self.cmd_publish(id, &args),
            b"hello" => self.cmd_hello(id, &args),
            b"auth" => self.cmd_auth(id, &args),
            b"client" => self.cmd_client(id, &args),
            b"command" => self.cmd_command(id, &args),
            b"role" => self.cmd_role(id),
            b"time" => self.cmd_time(id),
            b"config" => self.cmd_config(id, &args),
            b"info" => self.cmd_info(id),
            b"shutdown" => self.cmd_shutdown(id, &args),
            b"save" => self.cmd_save(id),
            b"rewriteaof" => self.cmd_rewrite(id),
            _ => {
                let session = self.sessions.get_mut(&id).unwrap();
                session.err("ERR unknown command");
            }
        }
    }
}

fn matches_any(name: &[u8], lits: &[&str]) -> bool {
    lits.iter().any(|lit| eq(name, lit))
}

fn is_known(name: &[u8]) -> bool {
    matches_any(
        name,
        &[
            "ping", "echo", "quit", "select", "get", "set", "setnx", "setex", "mget", "mset",
            "incr", "decr", "incrby", "decrby", "append", "strlen", "del", "exists", "type",
            "keys", "scan", "expire", "pexpire", "expireat", "pexpireat", "ttl", "pttl", "persist",
            "dbsize", "flushdb", "flushall", "lpush", "rpush", "lpop", "rpop", "llen", "lindex",
            "lrange", "hset", "hget", "hdel", "hexists", "hlen", "hgetall", "hkeys", "hvals",
            "sadd", "srem", "sismember", "smembers", "scard", "zadd", "zrem", "zscore", "zcard",
            "zrange", "zrank", "multi", "exec", "discard", "watch", "unwatch", "subscribe",
            "unsubscribe", "publish", "hello", "auth", "client", "command", "role", "time",
            "config", "info", "shutdown", "save", "rewriteaof",
        ],
    )
}

fn bad_syntax(name: &[u8], args: &[Vec<u8>]) -> bool {
    let n = args.len();
    if eq(name, "ping") {
        return n > 2;
    }
    if eq(name, "echo") || eq(name, "get") || eq(name, "ttl") || eq(name, "pttl") || eq(name, "persist")
        || eq(name, "type") || eq(name, "keys") || eq(name, "strlen") || eq(name, "select")
        || eq(name, "llen") || eq(name, "lpop") || eq(name, "rpop") || eq(name, "hlen")
        || eq(name, "hgetall") || eq(name, "hkeys") || eq(name, "hvals") || eq(name, "scard")
        || eq(name, "smembers") || eq(name, "zcard") || eq(name, "auth")
    {
        return n != 2;
    }
    if eq(name, "set") {
        return n < 3 || set_syntax(&args[3..]).is_err();
    }
    if eq(name, "setnx") || eq(name, "append") || eq(name, "hget") || eq(name, "hexists")
        || eq(name, "lindex") || eq(name, "sismember") || eq(name, "zscore") || eq(name, "zrank")
        || eq(name, "expire") || eq(name, "pexpire") || eq(name, "expireat") || eq(name, "pexpireat")
        || eq(name, "publish")
    {
        if n != 3 {
            return true;
        }
        if matches_any(name, &["expire", "pexpire", "expireat", "pexpireat", "lindex"]) {
            return parse_i64(&args[2]).is_none();
        }
        return false;
    }
    if eq(name, "setex") {
        return n != 4 || !matches!(parse_i64(&args[2]), Some(v) if v > 0);
    }
    if eq(name, "incr") || eq(name, "decr") {
        return n != 2;
    }
    if eq(name, "dbsize") || eq(name, "flushdb") || eq(name, "flushall") || eq(name, "multi")
        || eq(name, "exec") || eq(name, "discard") || eq(name, "unwatch") || eq(name, "role")
        || eq(name, "time") || eq(name, "save") || eq(name, "rewriteaof")
    {
        return n != 1;
    }
    if eq(name, "incrby") || eq(name, "decrby") {
        return n != 3 || parse_i64(&args[2]).is_none();
    }
    if eq(name, "mget") || eq(name, "del") || eq(name, "exists") || eq(name, "watch")
        || eq(name, "lpush") || eq(name, "rpush") || eq(name, "sadd") || eq(name, "srem")
        || eq(name, "subscribe") || eq(name, "hdel")
    {
        return n < 2;
    }
    if eq(name, "mset") || eq(name, "hset") {
        return n < 4 || n % 2 != 0;
    }
    if eq(name, "lrange") {
        return n != 4 || parse_i64(&args[2]).is_none() || parse_i64(&args[3]).is_none();
    }
    if eq(name, "zadd") {
        if n < 4 || n % 2 != 0 {
            return true;
        }
        return args.iter().skip(2).step_by(2).any(|s| parse_f64(s).is_none());
    }
    if eq(name, "zrem") || eq(name, "scan") {
        return n < 2;
    }
    if eq(name, "zrange") {
        return n < 4 || n > 5 || parse_i64(&args[2]).is_none() || parse_i64(&args[3]).is_none();
    }
    false
}

fn set_syntax(opts: &[Vec<u8>]) -> Result<(), ()> {
    let mut i = 0;
    let mut nx = false;
    let mut xx = false;
    let mut expire = false;
    while i < opts.len() {
        if eq(&opts[i], "nx") {
            if xx {
                return Err(());
            }
            nx = true;
            i += 1;
        } else if eq(&opts[i], "xx") {
            if nx {
                return Err(());
            }
            xx = true;
            i += 1;
        } else if eq(&opts[i], "get") {
            i += 1;
        } else if eq(&opts[i], "ex") || eq(&opts[i], "px") {
            if expire || i + 1 >= opts.len() || parse_i64(&opts[i + 1]).is_none() {
                return Err(());
            }
            expire = true;
            i += 2;
        } else {
            return Err(());
        }
    }
    Ok(())
}

fn syntax_error(session: &mut Session, name: &[u8], args: &[Vec<u8>]) {
    let lower = String::from_utf8_lossy(name).to_ascii_lowercase();
    if eq(name, "set") && args.len() >= 3 {
        session.err("ERR syntax error");
        return;
    }
    if eq(name, "setex") {
        session.err("ERR invalid expire time");
        return;
    }
    if matches_any(name, &["incrby", "decrby", "expire", "pexpire", "expireat", "pexpireat", "lindex", "lrange"])
        && parse_i64(args.last().unwrap_or(&Vec::new())).is_none()
    {
        session.err("ERR value is not an integer or out of range");
        return;
    }
    if eq(name, "zadd") && args.len() >= 4 && args.len() % 2 == 0 {
        session.err("ERR value is not a valid float");
        return;
    }
    session.err(&format!("ERR wrong number of arguments for '{lower}' command"));
}

fn write_argv(file: &mut File, args: &[Vec<u8>]) {
    let _ = write!(file, "*{}\r\n", args.len());
    for arg in args {
        let _ = write!(file, "${}\r\n", arg.len());
        let _ = file.write_all(arg);
        let _ = file.write_all(b"\r\n");
    }
}

impl Hub {
    fn cmd_ping(&mut self, id: u64, args: &[Vec<u8>]) {
        let session = self.sessions.get_mut(&id).unwrap();
        if session.pubsub {
            session.push(2);
            session.bulk(b"pong");
            if args.len() == 1 {
                session.bulk(b"");
            } else {
                session.bulk(&args[1]);
            }
            return;
        }
        if args.len() == 1 {
            session.simple("PONG");
        } else {
            session.bulk(&args[1]);
        }
    }

    fn cmd_select(&mut self, id: u64, args: &[Vec<u8>]) {
        let n = parse_i64(&args[1]).unwrap_or(-1);
        let session = self.sessions.get_mut(&id).unwrap();
        if n < 0 || n as usize >= self.dbs.len() {
            session.err("ERR DB index is out of range");
            return;
        }
        session.db = n as usize;
        session.simple("OK");
    }

    fn live<'a>(db: &'a mut Db, key: &[u8]) -> Option<&'a mut Entry> {
        Self::lookup(db, key)
    }

    fn wrong(session: &mut Session) {
        session.err("WRONGTYPE Operation against a key holding the wrong kind of value");
    }

    fn cmd_get(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let value = Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone());
        let session = self.sessions.get_mut(&id).unwrap();
        match value {
            Some(Value::Str(s)) => session.bulk(&s),
            Some(_) => Self::wrong(session),
            None => session.null(),
        }
    }

    fn cmd_set(&mut self, id: u64, args: &[Vec<u8>]) {
        let mut nx = false;
        let mut xx = false;
        let mut get = false;
        let mut expire_ms: Option<i64> = None;
        let mut i = 3;
        while i < args.len() {
            if eq(&args[i], "nx") {
                nx = true;
                i += 1;
            } else if eq(&args[i], "xx") {
                xx = true;
                i += 1;
            } else if eq(&args[i], "get") {
                get = true;
                i += 1;
            } else if eq(&args[i], "ex") {
                expire_ms = parse_i64(&args[i + 1]).map(|n| n * 1000);
                i += 2;
            } else if eq(&args[i], "px") {
                expire_ms = parse_i64(&args[i + 1]);
                i += 2;
            } else {
                i += 1;
            }
        }
        let db_i = self.sessions[&id].db;
        if self.maxmemory > 0 && self.used > self.maxmemory {
            self.sessions.get_mut(&id).unwrap().err("OOM command not allowed when used memory > 'maxmemory'");
            return;
        }
        let exists = Self::live(&mut self.dbs[db_i], &args[1]).is_some();
        if (nx && exists) || (xx && !exists) {
            self.sessions.get_mut(&id).unwrap().null();
            return;
        }
        let old = if get {
            Self::live(&mut self.dbs[db_i], &args[1]).and_then(|e| match &e.value {
                Value::Str(s) => Some(s.clone()),
                _ => None,
            })
        } else {
            None
        };
        let when = expire_ms.map(|ms| if ms <= 0 { now_ms() - 1 } else { now_ms() + ms });
        self.store_string(db_i, args[1].clone(), args[2].clone(), when);
        let session = self.sessions.get_mut(&id).unwrap();
        session.skip_aof = true;
        if get {
            if let Some(old) = old {
                session.bulk(&old);
            } else {
                session.null();
            }
        } else {
            session.simple("OK");
        }
        let key = args[1].clone();
        let val = args[2].clone();
        self.feed(db_i, &[b"SET".to_vec(), key.clone(), val]);
        if let Some(when) = when {
            self.feed(db_i, &[b"PEXPIREAT".to_vec(), key, when.to_string().into_bytes()]);
        }
    }

    fn store_string(&mut self, db_i: usize, key: Vec<u8>, val: Vec<u8>, expire: Option<i64>) {
        let db = &mut self.dbs[db_i];
        Self::touch(db, &key);
        Self::touch(db, &key);
        let entry = db.keys.entry(key).or_insert_with(|| Entry {
            value: Value::Str(Vec::new()),
            expire: None,
        });
        entry.value = Value::Str(val);
        entry.expire = expire;
    }

    fn cmd_setnx(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        if Self::live(&mut self.dbs[db_i], &args[1]).is_some() {
            self.sessions.get_mut(&id).unwrap().int(0);
            return;
        }
        self.store_string(db_i, args[1].clone(), args[2].clone(), None);
        self.feed(db_i, &[b"SETNX".to_vec(), args[1].clone(), args[2].clone()]);
        self.sessions.get_mut(&id).unwrap().int(1);
    }

    fn cmd_setex(&mut self, id: u64, args: &[Vec<u8>]) {
        let sec = parse_i64(&args[2]).unwrap_or(0);
        let db_i = self.sessions[&id].db;
        let when = now_ms() + sec * 1000;
        self.store_string(db_i, args[1].clone(), args[3].clone(), Some(when));
        self.feed(db_i, &[b"SET".to_vec(), args[1].clone(), args[3].clone()]);
        self.feed(db_i, &[b"PEXPIREAT".to_vec(), args[1].clone(), when.to_string().into_bytes()]);
        self.sessions.get_mut(&id).unwrap().simple("OK");
    }

    fn cmd_mget(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let mut vals = Vec::new();
        for key in &args[1..] {
            vals.push(match Self::live(&mut self.dbs[db_i], key).map(|e| e.value.clone()) {
                Some(Value::Str(s)) => Some(s),
                _ => None,
            });
        }
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(vals.len() as i64);
        for val in vals {
            if let Some(s) = val {
                session.bulk(&s);
            } else {
                session.null();
            }
        }
    }

    fn cmd_mset(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let mut i = 1;
        while i < args.len() {
            self.store_string(db_i, args[i].clone(), args[i + 1].clone(), None);
            i += 2;
        }
        self.feed_owned(db_i, args);
        self.sessions.get_mut(&id).unwrap().simple("OK");
    }

    fn feed_owned(&mut self, db: usize, args: &[Vec<u8>]) {
        self.feed(db, args);
    }

    fn cmd_incr(&mut self, id: u64, args: &[Vec<u8>], by: i64) {
        let db_i = self.sessions[&id].db;
        let cur = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Str(s)) => match parse_i64(&s) {
                Some(n) => n,
                None => {
                    self.sessions.get_mut(&id).unwrap().err("ERR value is not an integer or out of range");
                    return;
                }
            },
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        let overflow = if by > 0 {
            cur > i64::MAX - by
        } else if by < 0 {
            cur < i64::MIN - by
        } else {
            false
        };
        if overflow {
            self.sessions.get_mut(&id).unwrap().err("ERR increment or decrement would overflow");
            return;
        }
        let neu = cur + by;
        self.store_string(db_i, args[1].clone(), neu.to_string().into_bytes(), None);
        self.feed_owned(db_i, args);
        self.sessions.get_mut(&id).unwrap().int(neu);
    }

    fn cmd_incrby(&mut self, id: u64, args: &[Vec<u8>], neg: bool) {
        let mut by = parse_i64(&args[2]).unwrap_or(0);
        if neg {
            if by == i64::MIN {
                self.sessions.get_mut(&id).unwrap().err("ERR increment or decrement would overflow");
                return;
            }
            by = -by;
        }
        self.cmd_incr(id, args, by);
    }

    fn cmd_append(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let mut base = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Str(s)) => s,
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => Vec::new(),
        };
        base.extend_from_slice(&args[2]);
        let len = base.len() as i64;
        self.store_string(db_i, args[1].clone(), base, None);
        self.feed_owned(db_i, args);
        self.sessions.get_mut(&id).unwrap().int(len);
    }

    fn cmd_strlen(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Str(s)) => s.len() as i64,
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_del(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let mut n = 0;
        for key in &args[1..] {
            if self.dbs[db_i].keys.remove(key).is_some() {
                Self::touch(&mut self.dbs[db_i], key);
                n += 1;
            }
        }
        if n > 0 {
            self.feed_owned(db_i, args);
        }
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_exists(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = args[1..]
            .iter()
            .filter(|key| Self::live(&mut self.dbs[db_i], key).is_some())
            .count();
        self.sessions.get_mut(&id).unwrap().int(n as i64);
    }

    fn cmd_type(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let name = Self::live(&mut self.dbs[db_i], &args[1])
            .map(|e| Self::type_name(&e.value))
            .unwrap_or("none");
        self.sessions.get_mut(&id).unwrap().simple(name);
    }

    fn cmd_keys(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let keys: Vec<Vec<u8>> = self.dbs[db_i]
            .keys
            .iter()
            .filter(|(k, e)| !Self::expired(e) && glob(&args[1], k))
            .map(|(k, _)| k.clone())
            .collect();
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(keys.len() as i64);
        for key in keys {
            session.bulk(&key);
        }
    }

    fn cmd_scan(&mut self, id: u64, args: &[Vec<u8>]) {
        let mut cursor: usize = String::from_utf8_lossy(&args[1]).parse().unwrap_or(0);
        let mut count = 10usize;
        let mut pat: Option<Vec<u8>> = None;
        let mut i = 2;
        while i < args.len() {
            if eq(&args[i], "match") && i + 1 < args.len() {
                pat = Some(args[i + 1].clone());
                i += 2;
            } else if eq(&args[i], "count") && i + 1 < args.len() {
                count = parse_i64(&args[i + 1]).unwrap_or(10).max(1) as usize;
                i += 2;
            } else {
                self.sessions.get_mut(&id).unwrap().err("ERR syntax error");
                return;
            }
        }
        let db_i = self.sessions[&id].db;
        let mut keys: Vec<Vec<u8>> = self.dbs[db_i]
            .keys
            .iter()
            .filter(|(_, e)| !Self::expired(e))
            .map(|(k, _)| k.clone())
            .collect();
        keys.sort();
        if cursor > keys.len() {
            cursor = 0;
        }
        let end = (cursor + count).min(keys.len());
        let batch = keys[cursor..end].to_vec();
        let next = if end >= keys.len() { 0 } else { end };
        let filtered: Vec<Vec<u8>> = batch
            .into_iter()
            .filter(|k| pat.as_ref().map(|p| glob(p, k)).unwrap_or(true))
            .collect();
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(2);
        session.bulk(next.to_string().as_bytes());
        session.arr(filtered.len() as i64);
        for key in filtered {
            session.bulk(&key);
        }
    }

    fn cmd_expire(&mut self, id: u64, args: &[Vec<u8>]) {
        let name = args[0].to_ascii_lowercase();
        let ttl = parse_i64(&args[2]).unwrap_or(0);
        let absolute = name.ends_with(b"at");
        let millis = name.starts_with(b"p");
        let db_i = self.sessions[&id].db;
        if Self::live(&mut self.dbs[db_i], &args[1]).is_none() {
            self.sessions.get_mut(&id).unwrap().int(0);
            return;
        }
        let when = if !absolute {
            if ttl <= 0 {
                now_ms() - 1
            } else if millis {
                now_ms() + ttl
            } else {
                now_ms() + ttl * 1000
            }
        } else if millis {
            ttl
        } else {
            ttl * 1000
        };
        if when <= now_ms() {
            self.dbs[db_i].keys.remove(&args[1]);
            Self::touch(&mut self.dbs[db_i], &args[1]);
        } else if let Some(entry) = self.dbs[db_i].keys.get_mut(&args[1]) {
            entry.expire = Some(when);
            Self::touch(&mut self.dbs[db_i], &args[1]);
        }
        self.feed(db_i, &[b"PEXPIREAT".to_vec(), args[1].clone(), when.to_string().into_bytes()]);
        self.sessions.get_mut(&id).unwrap().int(1);
    }

    fn cmd_ttl(&mut self, id: u64, args: &[Vec<u8>], millis: bool) {
        let db_i = self.sessions[&id].db;
        let reply = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.expire) {
            None => -2,
            Some(None) => -1,
            Some(Some(when)) => {
                let left = (when - now_ms()).max(0);
                if millis { left } else { left / 1000 }
            }
        };
        self.sessions.get_mut(&id).unwrap().int(reply);
    }

    fn cmd_persist(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let had = Self::live(&mut self.dbs[db_i], &args[1]).and_then(|e| e.expire).is_some();
        if had {
            if let Some(entry) = self.dbs[db_i].keys.get_mut(&args[1]) {
                entry.expire = None;
            }
            Self::touch(&mut self.dbs[db_i], &args[1]);
            self.feed_owned(db_i, args);
        }
        self.sessions.get_mut(&id).unwrap().int(if had { 1 } else { 0 });
    }

    fn cmd_dbsize(&mut self, id: u64) {
        let db_i = self.sessions[&id].db;
        let n = self.dbs[db_i].keys.values().filter(|e| !Self::expired(e)).count();
        self.sessions.get_mut(&id).unwrap().int(n as i64);
    }

    fn cmd_flush(&mut self, id: u64, all: bool) {
        let db_i = self.sessions[&id].db;
        if all {
            for db in &mut self.dbs {
                db.keys.clear();
            }
        } else {
            self.dbs[db_i].keys.clear();
        }
        let flush_all = vec![b"FLUSHALL".to_vec()];
        let flush_db = vec![b"FLUSHDB".to_vec()];
        self.feed_owned(db_i, if all { &flush_all } else { &flush_db });
        self.sessions.get_mut(&id).unwrap().simple("OK");
    }

    fn cmd_push(&mut self, id: u64, args: &[Vec<u8>], head: bool) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        let wrong = matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(Value::Str(_)) | Some(Value::Hash(_)) | Some(Value::Set(_)) | Some(Value::Zset { .. }));
        if wrong {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        let list = match self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            Some(Value::List(list)) => list,
            _ => {
                self.dbs[db_i].keys.insert(key.clone(), Entry { value: Value::List(VecDeque::new()), expire: None });
                match &mut self.dbs[db_i].keys.get_mut(&key).unwrap().value {
                    Value::List(list) => list,
                    _ => unreachable!(),
                }
            }
        };
        for val in &args[2..] {
            if head {
                list.push_front(val.clone());
            } else {
                list.push_back(val.clone());
            }
        }
        let n = list.len() as i64;
        Self::touch(&mut self.dbs[db_i], &key);
        self.feed_owned(db_i, args);
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_pop(&mut self, id: u64, args: &[Vec<u8>], head: bool) {
        let db_i = self.sessions[&id].db;
        let key = &args[1];
        let popped = match Self::live(&mut self.dbs[db_i], key).map(|e| &mut e.value) {
            Some(Value::List(list)) => {
                let val = if head { list.pop_front() } else { list.pop_back() };
                let empty = list.is_empty();
                (val, empty, false)
            }
            Some(_) => (None, false, true),
            None => (None, false, false),
        };
        if popped.2 {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        if popped.1 {
            self.dbs[db_i].keys.remove(key);
        }
        if popped.0.is_some() {
            Self::touch(&mut self.dbs[db_i], key);
            self.feed_owned(db_i, args);
        }
        let session = self.sessions.get_mut(&id).unwrap();
        if let Some(val) = popped.0 {
            session.bulk(&val);
        } else {
            session.null();
        }
    }

    fn cmd_llen(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| &e.value) {
            Some(Value::List(list)) => {
                let n = list.len() as i64;
                self.sessions.get_mut(&id).unwrap().int(n);
            }
            Some(_) => Self::wrong(self.sessions.get_mut(&id).unwrap()),
            None => self.sessions.get_mut(&id).unwrap().int(0),
        }
    }

    fn cmd_lindex(&mut self, id: u64, args: &[Vec<u8>]) {
        let idx = parse_i64(&args[2]).unwrap_or(0);
        let db_i = self.sessions[&id].db;
        let item = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::List(list)) => {
                let len = list.len() as i64;
                let mut i = idx;
                if i < 0 {
                    i += len;
                }
                if i < 0 || i >= len { None } else { list.get(i as usize).cloned() }
            }
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => None,
        };
        let session = self.sessions.get_mut(&id).unwrap();
        if let Some(item) = item {
            session.bulk(&item);
        } else {
            session.null();
        }
    }

    fn cmd_lrange(&mut self, id: u64, args: &[Vec<u8>]) {
        let mut start = parse_i64(&args[2]).unwrap_or(0);
        let mut stop = parse_i64(&args[3]).unwrap_or(0);
        let db_i = self.sessions[&id].db;
        let list = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::List(list)) => list,
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => {
                self.sessions.get_mut(&id).unwrap().arr(0);
                return;
            }
        };
        let len = list.len() as i64;
        if start < 0 { start += len; }
        if stop < 0 { stop += len; }
        if start < 0 { start = 0; }
        if stop >= len { stop = len - 1; }
        if start > stop || start >= len {
            self.sessions.get_mut(&id).unwrap().arr(0);
            return;
        }
        let slice: Vec<Vec<u8>> = list.iter().skip(start as usize).take((stop - start + 1) as usize).cloned().collect();
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(slice.len() as i64);
        for item in slice {
            session.bulk(&item);
        }
    }

    fn cmd_hset(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        if matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(v) if !matches!(v, Value::Hash(_))) {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        if !self.dbs[db_i].keys.contains_key(&key) {
            self.dbs[db_i].keys.insert(key.clone(), Entry { value: Value::Hash(HashMap::new()), expire: None });
        }
        let mut added = 0i64;
        if let Some(Value::Hash(map)) = self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            let mut i = 2;
            while i < args.len() {
                if map.insert(args[i].clone(), args[i + 1].clone()).is_none() {
                    added += 1;
                }
                i += 2;
            }
        }
        Self::touch(&mut self.dbs[db_i], &key);
        self.feed_owned(db_i, args);
        self.sessions.get_mut(&id).unwrap().int(added);
    }

    fn cmd_hget(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let val = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Hash(map)) => map.get(&args[2]).cloned(),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => None,
        };
        let session = self.sessions.get_mut(&id).unwrap();
        if let Some(val) = val {
            session.bulk(&val);
        } else {
            session.null();
        }
    }

    fn cmd_hdel(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        let mut n = 0i64;
        let wrong = matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(v) if !matches!(v, Value::Hash(_)));
        if wrong {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        if let Some(Value::Hash(map)) = self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            for field in &args[2..] {
                if map.remove(field).is_some() {
                    n += 1;
                }
            }
        }
        if n > 0 {
            Self::touch(&mut self.dbs[db_i], &key);
            self.feed_owned(db_i, args);
        }
        if let Some(entry) = self.dbs[db_i].keys.get(&key) {
            if let Value::Hash(map) = &entry.value {
                if map.is_empty() {
                    self.dbs[db_i].keys.remove(&key);
                }
            }
        }
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_hexists(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| &e.value) {
            Some(Value::Hash(map)) => i64::from(map.contains_key(&args[2])),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_hlen(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| &e.value) {
            Some(Value::Hash(map)) => map.len() as i64,
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_hgetall(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let pairs = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Hash(map)) => map.into_iter().collect::<Vec<_>>(),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => Vec::new(),
        };
        let session = self.sessions.get_mut(&id).unwrap();
        session.map(pairs.len() as i64);
        for (k, v) in pairs {
            session.bulk(&k);
            session.bulk(&v);
        }
    }

    fn cmd_hkeys(&mut self, id: u64, args: &[Vec<u8>], keys: bool) {
        let db_i = self.sessions[&id].db;
        let items = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Hash(map)) => map.into_iter().map(|(k, v)| if keys { k } else { v }).collect::<Vec<_>>(),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => Vec::new(),
        };
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(items.len() as i64);
        for item in items {
            session.bulk(&item);
        }
    }

    fn cmd_sadd(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        if matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(v) if !matches!(v, Value::Set(_))) {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        if !self.dbs[db_i].keys.contains_key(&key) {
            self.dbs[db_i].keys.insert(key.clone(), Entry { value: Value::Set(HashMap::new()), expire: None });
        }
        let mut added = 0i64;
        if let Some(Value::Set(set)) = self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            for member in &args[2..] {
                if set.insert(member.clone(), ()).is_none() {
                    added += 1;
                }
            }
        }
        if added > 0 {
            Self::touch(&mut self.dbs[db_i], &key);
            self.feed_owned(db_i, args);
        }
        self.sessions.get_mut(&id).unwrap().int(added);
    }

    fn cmd_srem(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        if matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(v) if !matches!(v, Value::Set(_))) {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        let mut n = 0i64;
        if let Some(Value::Set(set)) = self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            for member in &args[2..] {
                if set.remove(member).is_some() {
                    n += 1;
                }
            }
        }
        if n > 0 {
            Self::touch(&mut self.dbs[db_i], &key);
            self.feed_owned(db_i, args);
        }
        if let Some(Value::Set(set)) = self.dbs[db_i].keys.get(&key).map(|e| &e.value) {
            if set.is_empty() {
                self.dbs[db_i].keys.remove(&key);
            }
        }
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_sismember(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| &e.value) {
            Some(Value::Set(set)) => i64::from(set.contains_key(&args[2])),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_smembers(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let members = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Set(set)) => set.into_keys().collect::<Vec<_>>(),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => Vec::new(),
        };
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(members.len() as i64);
        for member in members {
            session.bulk(&member);
        }
    }

    fn cmd_scard(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| &e.value) {
            Some(Value::Set(set)) => set.len() as i64,
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_zadd(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        if matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(v) if !matches!(v, Value::Zset { .. })) {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        if !matches!(self.dbs[db_i].keys.get(&key).map(|e| &e.value), Some(Value::Zset { .. })) {
            self.dbs[db_i].keys.insert(key.clone(), Entry {
                value: Value::Zset { order: Skiplist::new(), scores: HashMap::new() },
                expire: None,
            });
        }
        let mut added = 0i64;
        if let Some(Value::Zset { order, scores }) = self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            let mut i = 2;
            while i < args.len() {
                let score = parse_f64(&args[i]).unwrap_or(0.0);
                let member = args[i + 1].clone();
                if let Some(old) = scores.insert(member.clone(), score) {
                    order.delete(old, &member);
                    order.insert(score, member);
                } else {
                    order.insert(score, member);
                    added += 1;
                }
                i += 2;
            }
        }
        Self::touch(&mut self.dbs[db_i], &key);
        self.feed_owned(db_i, args);
        self.sessions.get_mut(&id).unwrap().int(added);
    }

    fn cmd_zrem(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let key = args[1].clone();
        if matches!(Self::live(&mut self.dbs[db_i], &key).map(|e| &e.value), Some(v) if !matches!(v, Value::Zset { .. })) {
            Self::wrong(self.sessions.get_mut(&id).unwrap());
            return;
        }
        let mut n = 0i64;
        if let Some(Value::Zset { order, scores }) = self.dbs[db_i].keys.get_mut(&key).map(|e| &mut e.value) {
            for member in &args[2..] {
                if let Some(score) = scores.remove(member) {
                    order.delete(score, member);
                    n += 1;
                }
            }
        }
        if n > 0 {
            Self::touch(&mut self.dbs[db_i], &key);
            self.feed_owned(db_i, args);
        }
        if let Some(Value::Zset { scores, .. }) = self.dbs[db_i].keys.get(&key).map(|e| &e.value) {
            if scores.is_empty() {
                self.dbs[db_i].keys.remove(&key);
            }
        }
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_zscore(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let score = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Zset { scores, .. }) => scores.get(&args[2]).copied(),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => None,
        };
        let session = self.sessions.get_mut(&id).unwrap();
        if let Some(score) = score {
            session.bulk(format!("{score:.17}").trim_end_matches('0').trim_end_matches('.').as_bytes());
        } else {
            session.null();
        }
    }

    fn cmd_zcard(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let n = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| &e.value) {
            Some(Value::Zset { scores, .. }) => scores.len() as i64,
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => 0,
        };
        self.sessions.get_mut(&id).unwrap().int(n);
    }

    fn cmd_zrange(&mut self, id: u64, args: &[Vec<u8>]) {
        let mut start = parse_i64(&args[2]).unwrap_or(0);
        let mut stop = parse_i64(&args[3]).unwrap_or(0);
        let withscores = args.len() == 5 && eq(&args[4], "withscores");
        let db_i = self.sessions[&id].db;
        let order = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Zset { order, .. }) => Some(order),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => None,
        };
        let Some(order) = order else {
            self.sessions.get_mut(&id).unwrap().arr(0);
            return;
        };
        let len = order.len() as i64;
        if start < 0 { start += len; }
        if stop < 0 { stop += len; }
        if start < 0 { start = 0; }
        if stop >= len { stop = len - 1; }
        if start > stop || start >= len {
            self.sessions.get_mut(&id).unwrap().arr(0);
            return;
        }
        let mut rows = Vec::new();
        for rank in start..=stop {
            if let Some((member, score)) = order.by_rank((rank as usize) + 1) {
                rows.push((member.to_vec(), score));
            }
        }
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr((rows.len() * if withscores { 2 } else { 1 }) as i64);
        for (member, score) in rows {
            session.bulk(&member);
            if withscores {
                let text = format!("{score:.17}");
                session.bulk(text.trim_end_matches('0').trim_end_matches('.').as_bytes());
            }
        }
    }

    fn cmd_zrank(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        let rank = match Self::live(&mut self.dbs[db_i], &args[1]).map(|e| e.value.clone()) {
            Some(Value::Zset { order, scores }) => scores.get(&args[2]).and_then(|score| order.rank(*score, &args[2])),
            Some(_) => {
                Self::wrong(self.sessions.get_mut(&id).unwrap());
                return;
            }
            None => None,
        };
        let session = self.sessions.get_mut(&id).unwrap();
        if let Some(rank) = rank {
            session.int(rank as i64 - 1);
        } else {
            session.null();
        }
    }

    fn cmd_multi(&mut self, id: u64) {
        let session = self.sessions.get_mut(&id).unwrap();
        if session.multi {
            session.err("ERR MULTI calls can not be nested");
            return;
        }
        session.multi = true;
        session.simple("OK");
    }

    fn cmd_discard(&mut self, id: u64) {
        let session = self.sessions.get_mut(&id).unwrap();
        if !session.multi {
            session.err("ERR DISCARD without MULTI");
            return;
        }
        session.multi = false;
        session.queue.clear();
        session.dirty_exec = false;
        session.dirty_cas = false;
        session.watched.clear();
        session.simple("OK");
    }

    fn cmd_exec(&mut self, id: u64) {
        let (db_i, watched, already_dirty) = {
            let session = self.sessions.get(&id).unwrap();
            if !session.multi {
                let session = self.sessions.get_mut(&id).unwrap();
                session.err("ERR EXEC without MULTI");
                return;
            }
            (session.db, session.watched.clone(), session.dirty_cas)
        };
        let dirty_cas = already_dirty
            || watched.iter().any(|(key, seen)| {
                self.dbs[db_i].touch.get(key).copied().unwrap_or(0) != *seen
            });
        let session = self.sessions.get_mut(&id).unwrap();
        let queue = std::mem::take(&mut session.queue);
        let dirty_exec = session.dirty_exec;
        session.multi = false;
        session.dirty_exec = false;
        session.dirty_cas = false;
        session.watched.clear();
        if dirty_exec {
            session.err("EXECABORT Transaction discarded because of previous errors.");
            return;
        }
        if dirty_cas {
            session.null_array();
            return;
        }
        let n = queue.len() as i64;
        self.sessions.get_mut(&id).unwrap().arr(n);
        for args in queue {
            self.dispatch(id, args);
        }
    }

    fn cmd_watch(&mut self, id: u64, args: &[Vec<u8>]) {
        let db_i = self.sessions[&id].db;
        if self.sessions[&id].multi {
            let session = self.sessions.get_mut(&id).unwrap();
            session.err("ERR WATCH inside MULTI is not allowed");
            session.dirty_exec = true;
            return;
        }
        for key in &args[1..] {
            let gen = self.dbs[db_i].touch.get(key).copied().unwrap_or(0);
            self.sessions.get_mut(&id).unwrap().watched.insert(key.clone(), gen);
        }
        self.sessions.get_mut(&id).unwrap().simple("OK");
    }

    fn cmd_unwatch(&mut self, id: u64) {
        let session = self.sessions.get_mut(&id).unwrap();
        session.watched.clear();
        session.dirty_cas = false;
        session.simple("OK");
    }

    fn cmd_subscribe(&mut self, id: u64, args: &[Vec<u8>]) {
        for channel in &args[1..] {
            let list = self.channels.entry(channel.clone()).or_default();
            if !list.contains(&id) {
                list.push(id);
            }
            let session = self.sessions.get_mut(&id).unwrap();
            if !session.subs.iter().any(|s| s == channel) {
                session.subs.push(channel.clone());
            }
            session.pubsub = true;
            let n = session.subs.len() as i64;
            session.push(3);
            session.bulk(b"subscribe");
            session.bulk(channel);
            session.int(n);
        }
    }

    fn cmd_unsubscribe(&mut self, id: u64, args: &[Vec<u8>]) {
        let names = if args.len() == 1 {
            self.sessions[&id].subs.clone()
        } else {
            args[1..].to_vec()
        };
        if names.is_empty() {
            let session = self.sessions.get_mut(&id).unwrap();
            session.push(3);
            session.bulk(b"unsubscribe");
            session.null();
            session.int(0);
            return;
        }
        for channel in names {
            if let Some(list) = self.channels.get_mut(&channel) {
                list.retain(|cid| *cid != id);
            }
            let session = self.sessions.get_mut(&id).unwrap();
            session.subs.retain(|s| s != &channel);
            if session.subs.is_empty() {
                session.pubsub = false;
            }
            let n = session.subs.len() as i64;
            session.push(3);
            session.bulk(b"unsubscribe");
            session.bulk(&channel);
            session.int(n);
        }
    }

    fn cmd_publish(&mut self, id: u64, args: &[Vec<u8>]) {
        let subs = self.channels.get(&args[1]).cloned().unwrap_or_default();
        for sid in &subs {
            if let Some(session) = self.sessions.get_mut(sid) {
                session.push(3);
                session.bulk(b"message");
                session.bulk(&args[1]);
                session.bulk(&args[2]);
            }
        }
        self.sessions.get_mut(&id).unwrap().int(subs.len() as i64);
    }

    fn cmd_hello(&mut self, id: u64, args: &[Vec<u8>]) {
        let mut proto = self.sessions[&id].proto as i64;
        if args.len() >= 2 {
            proto = parse_i64(&args[1]).unwrap_or(-1);
            if proto != 2 && proto != 3 {
                self.sessions.get_mut(&id).unwrap().err("NOPROTO unsupported protocol version");
                return;
            }
            let mut i = 2;
            while i < args.len() {
                if eq(&args[i], "auth") && i + 1 < args.len() {
                    let pass = &args[i + 1];
                    if self.requirepass.is_empty() {
                        self.sessions.get_mut(&id).unwrap().err("ERR AUTH called without any password configured for the default user");
                        return;
                    }
                    if pass != &self.requirepass {
                        self.sessions.get_mut(&id).unwrap().err("WRONGPASS invalid username-password pair or user is disabled");
                        return;
                    }
                    self.sessions.get_mut(&id).unwrap().authed = true;
                    i += 2;
                } else if eq(&args[i], "setname") && i + 1 < args.len() {
                    self.sessions.get_mut(&id).unwrap().name = Some(args[i + 1].clone());
                    i += 2;
                } else {
                    self.sessions.get_mut(&id).unwrap().err("ERR syntax error");
                    return;
                }
            }
            self.sessions.get_mut(&id).unwrap().proto = proto as i32;
        }
        let session = self.sessions.get_mut(&id).unwrap();
        let idn = session.id;
        session.map(7);
        for (k, v) in [("server", "redis"), ("version", "7.2.4")] {
            session.bulk(k.as_bytes());
            session.bulk(v.as_bytes());
        }
        session.bulk(b"proto");
        session.int(proto);
        session.bulk(b"id");
        session.int(idn as i64);
        session.bulk(b"mode");
        session.bulk(b"standalone");
        session.bulk(b"role");
        session.bulk(b"master");
        session.bulk(b"modules");
        session.arr(0);
    }

    fn cmd_auth(&mut self, id: u64, args: &[Vec<u8>]) {
        let session = self.sessions.get_mut(&id).unwrap();
        if self.requirepass.is_empty() {
            session.err("ERR AUTH called without any password configured for the default user");
            return;
        }
        if args[1] == self.requirepass {
            session.authed = true;
            session.simple("OK");
        } else {
            session.err("WRONGPASS invalid username-password pair or user is disabled");
        }
    }

    fn cmd_client(&mut self, id: u64, args: &[Vec<u8>]) {
        let session = self.sessions.get_mut(&id).unwrap();
        if eq(&args[1], "id") && args.len() == 2 {
            let n = session.id as i64;
            session.int(n);
        } else if eq(&args[1], "getname") && args.len() == 2 {
            if let Some(name) = session.name.clone() {
                if name.is_empty() { session.null(); } else { session.bulk(&name); }
            } else {
                session.null();
            }
        } else if eq(&args[1], "setname") && args.len() == 3 {
            session.name = Some(args[2].clone());
            session.simple("OK");
        } else if eq(&args[1], "setinfo") && args.len() == 4 && (eq(&args[2], "lib-name") || eq(&args[2], "lib-ver")) {
            session.simple("OK");
        } else {
            session.err("ERR unknown subcommand");
        }
    }

    fn cmd_command(&mut self, id: u64, args: &[Vec<u8>]) {
        let session = self.sessions.get_mut(&id).unwrap();
        if args.len() == 1 || (args.len() == 2 && eq(&args[1], "count")) {
            session.int(40);
            return;
        }
        if args.len() >= 3 && eq(&args[1], "info") {
            session.arr((args.len() - 2) as i64);
            for name in &args[2..] {
                if is_known(name) {
                    session.arr(6);
                    session.bulk(&name.to_ascii_lowercase());
                    session.int(-1);
                    session.arr(0);
                    session.int(0);
                    session.int(0);
                    session.int(0);
                } else {
                    session.null();
                }
            }
            return;
        }
        session.err("ERR unknown subcommand");
    }

    fn cmd_role(&mut self, id: u64) {
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(3);
        session.bulk(b"master");
        session.int(0);
        session.arr(0);
    }

    fn cmd_time(&mut self, id: u64) {
        let ms = now_ms();
        let session = self.sessions.get_mut(&id).unwrap();
        session.arr(2);
        session.bulk((ms / 1000).to_string().as_bytes());
        session.bulk(((ms % 1000) * 1000).to_string().as_bytes());
    }

    fn cmd_config(&mut self, id: u64, args: &[Vec<u8>]) {
        if args.len() >= 2 && eq(&args[1], "get") && args.len() == 3 {
            let pairs = [
                ("appendonly", if self.appendonly { "yes" } else { "no" }),
                ("appendfilename", self.aof_name.as_str()),
                ("dbfilename", self.db_name.as_str()),
            ];
            let matched: Vec<(&str, &str)> = pairs.into_iter().filter(|(k, _)| glob(&args[2], k.as_bytes())).collect();
            let session = self.sessions.get_mut(&id).unwrap();
            session.map(matched.len() as i64);
            for (k, v) in matched {
                session.bulk(k.as_bytes());
                session.bulk(v.as_bytes());
            }
            return;
        }
        if args.len() == 4 && eq(&args[1], "set") && eq(&args[2], "timeout") {
            if let Some(n) = parse_i64(&args[3]) {
                self.timeout_secs = n as u64;
                self.sessions.get_mut(&id).unwrap().simple("OK");
                return;
            }
        }
        if args.len() == 4 && eq(&args[1], "set") && eq(&args[2], "appendfsync") {
            self.sessions.get_mut(&id).unwrap().simple("OK");
            return;
        }
        let session = self.sessions.get_mut(&id).unwrap();
        if args.len() >= 3 && eq(&args[1], "set") {
            session.err(&format!("ERR Unsupported CONFIG parameter: {}", String::from_utf8_lossy(&args[2])));
        } else {
            session.err("ERR unknown subcommand");
        }
    }

    fn cmd_info(&mut self, id: u64) {
        let text = format!(
            "# Server\r\nredis_version:7.2.4\r\nsimpleredis_version:0.1.0\r\ntcp_port:{}\r\nrole:master\r\n# Clients\r\nconnected_clients:{}\r\n# Memory\r\nused_memory:{}\r\n# Persistence\r\naof_enabled:{}\r\n# Stats\r\ntotal_commands_processed:{}\r\n# Keyspace\r\n",
            self.port,
            self.sessions.len(),
            self.used,
            i32::from(self.appendonly),
            self.commands
        );
        self.sessions.get_mut(&id).unwrap().bulk(text.as_bytes());
    }

    fn cmd_shutdown(&mut self, id: u64, args: &[Vec<u8>]) {
        if args.len() == 2 && eq(&args[1], "nosave") {
            self.no_save = true;
        } else if args.len() == 2 && eq(&args[1], "save") {
            self.force_save = true;
        } else if args.len() > 2 {
            self.sessions.get_mut(&id).unwrap().err("ERR syntax error");
            return;
        }
        self.sessions.get_mut(&id).unwrap().simple("OK");
        self.shutdown = true;
    }

    fn cmd_save(&mut self, id: u64) {
        if self.save_snapshot().is_ok() {
            self.sessions.get_mut(&id).unwrap().simple("OK");
        } else {
            self.sessions.get_mut(&id).unwrap().err("ERR save failed");
        }
    }

    fn cmd_rewrite(&mut self, id: u64) {
        if !self.appendonly {
            self.sessions.get_mut(&id).unwrap().err("ERR AOF is disabled");
            return;
        }
        self.sessions.get_mut(&id).unwrap().simple("OK");
    }

    pub fn save_snapshot(&mut self) -> std::io::Result<()> {
        let path = self.path(&self.db_name);
        let tmp = path.with_extension("srdb.tmp");
        let mut body = Vec::new();
        body.extend_from_slice(b"SRDB");
        body.extend_from_slice(&1u32.to_le_bytes());
        let mut dbcount = 0u32;
        let mut payload = Vec::new();
        for (index, db) in self.dbs.iter().enumerate() {
            let live: Vec<(&Vec<u8>, &Entry)> = db.keys.iter().filter(|(_, e)| !Self::expired(e)).collect();
            if live.is_empty() {
                continue;
            }
            dbcount += 1;
            payload.extend_from_slice(&(index as u32).to_le_bytes());
            payload.extend_from_slice(&(live.len() as u32).to_le_bytes());
            for (key, entry) in live {
                let expire = entry.expire.unwrap_or(-1);
                payload.extend_from_slice(&expire.to_le_bytes());
                let type_byte = match &entry.value {
                    Value::Str(_) => 0u8,
                    Value::List(_) => 1,
                    Value::Hash(_) => 2,
                    Value::Set(_) => 3,
                    Value::Zset { .. } => 4,
                };
                payload.push(type_byte);
                write_str(&mut payload, key);
                match &entry.value {
                    Value::Str(s) => write_str(&mut payload, s),
                    Value::List(list) => {
                        payload.extend_from_slice(&(list.len() as u32).to_le_bytes());
                        for item in list {
                            write_str(&mut payload, item);
                        }
                    }
                    Value::Hash(map) => {
                        payload.extend_from_slice(&(map.len() as u32).to_le_bytes());
                        for (f, v) in map {
                            write_str(&mut payload, f);
                            write_str(&mut payload, v);
                        }
                    }
                    Value::Set(set) => {
                        payload.extend_from_slice(&(set.len() as u32).to_le_bytes());
                        for member in set.keys() {
                            write_str(&mut payload, member);
                        }
                    }
                    Value::Zset { order, .. } => {
                        payload.extend_from_slice(&(order.len() as u32).to_le_bytes());
                        for rank in 1..=order.len() {
                            if let Some((member, score)) = order.by_rank(rank) {
                                write_str(&mut payload, member);
                                payload.extend_from_slice(&score.to_le_bytes());
                            }
                        }
                    }
                }
            }
        }
        body.extend_from_slice(&dbcount.to_le_bytes());
        body.extend_from_slice(&payload);
        fs::write(&tmp, &body)?;
        fs::rename(tmp, path)?;
        Ok(())
    }

    pub fn load_snapshot(&mut self) -> std::io::Result<()> {
        let path = self.path(&self.db_name);
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(err) => return Err(err),
        };
        if bytes.len() < 12 || &bytes[..4] != b"SRDB" {
            return Err(std::io::Error::other("bad snapshot"));
        }
        let mut off = 4;
        let _version = read_u32(&bytes, &mut off).ok_or_else(|| std::io::Error::other("short"))?;
        let dbcount = read_u32(&bytes, &mut off).ok_or_else(|| std::io::Error::other("short"))?;
        self.loading = true;
        for _ in 0..dbcount {
            let index = read_u32(&bytes, &mut off).ok_or_else(|| std::io::Error::other("short"))? as usize;
            let keys = read_u32(&bytes, &mut off).ok_or_else(|| std::io::Error::other("short"))?;
            for _ in 0..keys {
                let expire = read_i64(&bytes, &mut off).ok_or_else(|| std::io::Error::other("short"))?;
                let type_byte = *bytes.get(off).ok_or_else(|| std::io::Error::other("short"))?;
                off += 1;
                let key = read_str(&bytes, &mut off).ok_or_else(|| std::io::Error::other("short"))?;
                let value = read_value(&bytes, &mut off, type_byte).ok_or_else(|| std::io::Error::other("short"))?;
                if expire != -1 && expire <= now_ms() {
                    continue;
                }
                if index < self.dbs.len() {
                    self.dbs[index].keys.insert(key, Entry { value, expire: if expire == -1 { None } else { Some(expire) } });
                }
            }
        }
        self.loading = false;
        Ok(())
    }

    pub fn shutdown_persist(&mut self) {
        if self.no_save {
            return;
        }
        if self.force_save || !self.appendonly {
            let _ = self.save_snapshot();
        }
        if let Some(file) = self.aof.as_mut() {
            let _ = file.flush();
            let _ = file.sync_all();
        }
    }
}

fn write_str(out: &mut Vec<u8>, data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
}

fn read_u32(buf: &[u8], off: &mut usize) -> Option<u32> {
    let bytes: [u8; 4] = buf.get(*off..*off + 4)?.try_into().ok()?;
    *off += 4;
    Some(u32::from_le_bytes(bytes))
}

fn read_i64(buf: &[u8], off: &mut usize) -> Option<i64> {
    let bytes: [u8; 8] = buf.get(*off..*off + 8)?.try_into().ok()?;
    *off += 8;
    Some(i64::from_le_bytes(bytes))
}

fn read_f64(buf: &[u8], off: &mut usize) -> Option<f64> {
    let bytes: [u8; 8] = buf.get(*off..*off + 8)?.try_into().ok()?;
    *off += 8;
    Some(f64::from_le_bytes(bytes))
}

fn read_str(buf: &[u8], off: &mut usize) -> Option<Vec<u8>> {
    let n = read_u32(buf, off)? as usize;
    let data = buf.get(*off..*off + n)?.to_vec();
    *off += n;
    Some(data)
}

fn read_value(buf: &[u8], off: &mut usize, type_byte: u8) -> Option<Value> {
    match type_byte {
        0 => Some(Value::Str(read_str(buf, off)?)),
        1 => {
            let n = read_u32(buf, off)? as usize;
            let mut list = VecDeque::new();
            for _ in 0..n {
                list.push_back(read_str(buf, off)?);
            }
            Some(Value::List(list))
        }
        2 => {
            let n = read_u32(buf, off)? as usize;
            let mut map = HashMap::new();
            for _ in 0..n {
                map.insert(read_str(buf, off)?, read_str(buf, off)?);
            }
            Some(Value::Hash(map))
        }
        3 => {
            let n = read_u32(buf, off)? as usize;
            let mut set = HashMap::new();
            for _ in 0..n {
                set.insert(read_str(buf, off)?, ());
            }
            Some(Value::Set(set))
        }
        4 => {
            let n = read_u32(buf, off)? as usize;
            let mut order = Skiplist::new();
            let mut scores = HashMap::new();
            for _ in 0..n {
                let member = read_str(buf, off)?;
                let score = read_f64(buf, off)?;
                scores.insert(member.clone(), score);
                order.insert(score, member);
            }
            Some(Value::Zset { order, scores })
        }
        _ => None,
    }
}

pub fn load_config(path: &Path) -> (Hub, u16) {
    let text = fs::read_to_string(path).unwrap_or_default();
    let mut hub = Hub::new(16);
    let mut port = 6379u16;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut parts = line.splitn(2, char::is_whitespace);
        let key = parts.next().unwrap_or("");
        let val = parts.next().unwrap_or("").trim();
        match key {
            "port" => port = val.parse().unwrap_or(6379),
            "databases" => hub = Hub::new(val.parse().unwrap_or(16)),
            "appendonly" => hub.appendonly = val.eq_ignore_ascii_case("yes"),
            "dir" => hub.dir = PathBuf::from(val),
            "appendfilename" => hub.aof_name = val.to_string(),
            "dbfilename" => hub.db_name = val.to_string(),
            "requirepass" => hub.requirepass = val.as_bytes().to_vec(),
            "timeout" => hub.timeout_secs = val.parse().unwrap_or(0),
            "maxmemory" => hub.maxmemory = val.parse().unwrap_or(0),
            _ => {}
        }
    }
    hub.port = port;
    (hub, port)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_get_and_overflow() {
        let mut hub = Hub::new(16);
        let id = hub.add_session();
        hub.exec(id, vec![b"SET".to_vec(), b"k".to_vec(), b"v".to_vec()]);
        assert_eq!(hub.take_reply(id), b"+OK\r\n");
        hub.exec(id, vec![b"GET".to_vec(), b"k".to_vec()]);
        assert_eq!(hub.take_reply(id), b"$1\r\nv\r\n");
        hub.exec(id, vec![b"SET".to_vec(), b"big".to_vec(), b"9223372036854775807".to_vec()]);
        hub.take_reply(id);
        hub.exec(id, vec![b"INCR".to_vec(), b"big".to_vec()]);
        assert_eq!(hub.take_reply(id), b"-ERR increment or decrement would overflow\r\n");
        hub.exec(id, vec![b"HELLO".to_vec(), b"3".to_vec()]);
        let hello = hub.take_reply(id);
        assert!(hello.windows(4).any(|w| w == b"%7\r\n") || hello.starts_with(b"%"));
        hub.maxmemory = 1;
        hub.used = 2;
        hub.exec(id, vec![b"SET".to_vec(), b"n".to_vec(), b"12345678".to_vec()]);
        let reply = String::from_utf8_lossy(&hub.take_reply(id)).into_owned();
        assert!(reply.starts_with("+OK") || reply.contains("OOM"), "{reply}");
    }
}
