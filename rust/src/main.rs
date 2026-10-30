mod engine;
mod net;
mod resp;
mod siphash;
mod skiplist;

use std::env;
use std::path::Path;

fn main() {
    let mut args = env::args();
    let _bin = args.next();
    let config = args.next().unwrap_or_else(|| "redis.conf".to_string());
    net::serve(Path::new(&config));
}
