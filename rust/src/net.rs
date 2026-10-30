use crate::engine::{load_config, Hub};
use crate::resp::{self, Value};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::os::fd::{AsRawFd, FromRawFd};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

static STOP: AtomicBool = AtomicBool::new(false);

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct EpollEvent {
    events: u32,
    data: u64,
}

extern "C" {
    fn epoll_create1(flags: i32) -> i32;
    fn epoll_ctl(epfd: i32, op: i32, fd: i32, event: *mut EpollEvent) -> i32;
    fn epoll_wait(epfd: i32, events: *mut EpollEvent, maxevents: i32, timeout: i32) -> i32;
    fn signal(sig: i32, handler: usize) -> usize;
}

const EPOLLIN: u32 = 1;
const EPOLL_CTL_ADD: i32 = 1;
const EPOLL_CTL_MOD: i32 = 3;
const EPOLL_CLOEXEC: i32 = 0x80000;

extern "C" fn on_signal(_: i32) {
    STOP.store(true, Ordering::Relaxed);
}

struct Link {
    id: u64,
    query: Vec<u8>,
}

pub fn serve(config: &Path) {
    let (mut hub, port) = load_config(config);
    hub.open_aof();
    if hub.appendonly {
        hub.load_aof();
    } else {
        let _ = hub.load_snapshot();
    }
    let listener = TcpListener::bind(("0.0.0.0", port)).or_else(|_| TcpListener::bind(("127.0.0.1", port)));
    let listener = match listener {
        Ok(listener) => listener,
        Err(err) => {
            eprintln!("bind failed: {err}");
            std::process::exit(1);
        }
    };
    listener.set_nonblocking(true).ok();
    let bound = listener.local_addr().map(|a| a.port()).unwrap_or(port);
    println!("ready {bound}");
    let _ = std::io::stdout().flush();
    unsafe {
        signal(2, on_signal as usize);
        signal(15, on_signal as usize);
    }
    let epfd = unsafe { epoll_create1(EPOLL_CLOEXEC) };
    if epfd < 0 {
        eprintln!("epoll_create failed");
        std::process::exit(1);
    }
    let lfd = listener.as_raw_fd();
    add_fd(epfd, lfd, EPOLLIN);
    let mut links: HashMap<i32, Link> = HashMap::new();
    let mut events = [EpollEvent { events: 0, data: 0 }; 128];
    while !STOP.load(Ordering::Relaxed) && !hub.shutdown {
        let n = unsafe { epoll_wait(epfd, events.as_mut_ptr(), 128, 100) };
        if n < 0 {
            continue;
        }
        for event in events.iter().take(n as usize) {
            let fd = event.data as i32;
            if fd == lfd {
                while let Ok((stream, _)) = listener.accept() {
                    stream.set_nonblocking(true).ok();
                    let fd = stream.as_raw_fd();
                    let id = hub.add_session();
                    add_fd(epfd, fd, EPOLLIN);
                    links.insert(fd, Link { id, query: Vec::new() });
                    std::mem::forget(stream);
                }
            } else if links.contains_key(&fd) && event.events & EPOLLIN != 0 {
                let id = links[&fd].id;
                let mut buf = [0u8; 16384];
                let mut stream = unsafe { std::net::TcpStream::from_raw_fd(fd) };
                let read = stream.read(&mut buf);
                match read {
                    Ok(0) => {
                        drop(stream);
                        hub.sessions.remove(&id);
                        links.remove(&fd);
                        continue;
                    }
                    Err(err) if err.kind() != std::io::ErrorKind::WouldBlock => {
                        drop(stream);
                        hub.sessions.remove(&id);
                        links.remove(&fd);
                        continue;
                    }
                    _ => std::mem::forget(stream),
                }
                if let Ok(n) = read {
                    if n > 0 {
                        links.get_mut(&fd).unwrap().query.extend_from_slice(&buf[..n]);
                    }
                }
                let mut query = std::mem::take(&mut links.get_mut(&fd).unwrap().query);
                loop {
                    match resp::parse(&query) {
                        resp::Parse::NeedMore => break,
                        resp::Parse::Error => {
                            hub.sessions
                                .get_mut(&id)
                                .unwrap()
                                .reply
                                .extend(resp::error("ERR Protocol error"));
                            query.clear();
                            break;
                        }
                        resp::Parse::Done { value, consumed } => {
                            query.drain(..consumed);
                            if let Value::Array(items) = value {
                                let args = items
                                    .into_iter()
                                    .filter_map(|item| match item {
                                        Value::Bulk(bytes) => Some(bytes),
                                        _ => None,
                                    })
                                    .collect::<Vec<_>>();
                                hub.exec(id, args);
                            }
                        }
                    }
                }
                if let Some(link) = links.get_mut(&fd) {
                    link.query = query;
                }
                flush_replies(&mut hub, &links);
                if hub.shutdown {
                    break;
                }
            }
        }
    }
    hub.shutdown_persist();
    let _ = listener;
}

fn flush_replies(hub: &mut Hub, links: &HashMap<i32, Link>) {
    let pending: Vec<(i32, u64)> = links.iter().map(|(fd, link)| (*fd, link.id)).collect();
    for (fd, id) in pending {
        let reply = hub.take_reply(id);
        if reply.is_empty() {
            continue;
        }
        let mut stream = unsafe { std::net::TcpStream::from_raw_fd(fd) };
        let _ = stream.write_all(&reply);
        std::mem::forget(stream);
    }
}

fn add_fd(epfd: i32, fd: i32, events: u32) {
    let mut event = EpollEvent { events, data: fd as u64 };
    unsafe {
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &mut event);
    }
}

#[allow(dead_code)]
fn mod_fd(epfd: i32, fd: i32, events: u32) {
    let mut event = EpollEvent { events, data: fd as u64 };
    unsafe {
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &mut event);
    }
}
