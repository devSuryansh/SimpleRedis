#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Simple(String),
    Error(String),
    Int(i64),
    Bulk(Vec<u8>),
    Null,
    Array(Vec<Value>),
    Map(Vec<(Value, Value)>),
    Push(Vec<Value>),
}

#[derive(Debug)]
pub enum Parse {
    NeedMore,
    Done { value: Value, consumed: usize },
    Error,
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
    if rest.is_empty() {
        return None;
    }
    let mut acc: u64 = 0;
    for byte in rest {
        if !byte.is_ascii_digit() {
            return None;
        }
        let digit = u64::from(byte - b'0');
        acc = acc.checked_mul(10)?.checked_add(digit)?;
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

fn find_line(buf: &[u8], pos: usize) -> Result<Option<(usize, usize)>, ()> {
    let mut i = pos;
    while i < buf.len() {
        if buf[i] == b'\n' {
            if i == pos || buf[i - 1] != b'\r' {
                return Err(());
            }
            return Ok(Some((pos, i - pos - 1)));
        }
        i += 1;
    }
    Ok(None)
}

fn parse_at(buf: &[u8], pos: &mut usize) -> Result<Option<Value>, ()> {
    if *pos >= buf.len() {
        return Ok(None);
    }
    let kind = buf[*pos];
    let line = match find_line(buf, *pos + 1)? {
        Some((start, len)) => (start, len, start + len + 2),
        None => return Ok(None),
    };
    let text = &buf[line.0..line.0 + line.1];
    let next = line.2;
    match kind {
        b'+' => {
            *pos = next;
            Ok(Some(Value::Simple(String::from_utf8_lossy(text).into_owned())))
        }
        b'-' => {
            *pos = next;
            Ok(Some(Value::Error(String::from_utf8_lossy(text).into_owned())))
        }
        b':' => {
            let n = parse_i64(text).ok_or(())?;
            *pos = next;
            Ok(Some(Value::Int(n)))
        }
        b'_' => {
            *pos = next;
            Ok(Some(Value::Null))
        }
        b'$' => {
            let bulk = parse_i64(text).ok_or(())?;
            if bulk < -1 {
                return Err(());
            }
            if bulk == -1 {
                *pos = next;
                return Ok(Some(Value::Null));
            }
            if bulk > 64 * 1024 * 1024 {
                return Err(());
            }
            let bulk = bulk as usize;
            if next + bulk + 2 > buf.len() {
                return Ok(None);
            }
            if buf[next + bulk] != b'\r' || buf[next + bulk + 1] != b'\n' {
                return Err(());
            }
            let data = buf[next..next + bulk].to_vec();
            *pos = next + bulk + 2;
            Ok(Some(Value::Bulk(data)))
        }
        b'*' | b'%' | b'>' => {
            let count = parse_i64(text).ok_or(())?;
            if count == -1 && kind == b'*' {
                *pos = next;
                return Ok(Some(Value::Null));
            }
            if count < 0 {
                return Err(());
            }
            let n = if kind == b'%' {
                count.checked_mul(2).ok_or(())? as usize
            } else {
                count as usize
            };
            *pos = next;
            let mut items = Vec::with_capacity(n);
            for _ in 0..n {
                match parse_at(buf, pos)? {
                    Some(v) => items.push(v),
                    None => return Ok(None),
                }
            }
            Ok(Some(match kind {
                b'%' => {
                    let mut pairs = Vec::new();
                    let mut it = items.into_iter();
                    while let (Some(k), Some(v)) = (it.next(), it.next()) {
                        pairs.push((k, v));
                    }
                    Value::Map(pairs)
                }
                b'>' => Value::Push(items),
                _ => Value::Array(items),
            }))
        }
        _ => Err(()),
    }
}

pub fn parse(buf: &[u8]) -> Parse {
    let mut pos = 0;
    match parse_at(buf, &mut pos) {
        Ok(Some(value)) => Parse::Done { value, consumed: pos },
        Ok(None) => Parse::NeedMore,
        Err(()) => Parse::Error,
    }
}

pub fn bulk(data: &[u8]) -> Vec<u8> {
    let mut out = format!("${}\r\n", data.len()).into_bytes();
    out.extend_from_slice(data);
    out.extend_from_slice(b"\r\n");
    out
}

pub fn simple(text: &str) -> Vec<u8> {
    format!("+{text}\r\n").into_bytes()
}

pub fn error(text: &str) -> Vec<u8> {
    let body = if text.starts_with('-') {
        format!("{text}\r\n")
    } else {
        format!("-{text}\r\n")
    };
    body.into_bytes()
}

pub fn integer(n: i64) -> Vec<u8> {
    format!(":{n}\r\n").into_bytes()
}

pub fn null(proto: i32) -> Vec<u8> {
    if proto >= 3 {
        b"_\r\n".to_vec()
    } else {
        b"$-1\r\n".to_vec()
    }
}

pub fn null_array(proto: i32) -> Vec<u8> {
    if proto >= 3 {
        b"_\r\n".to_vec()
    } else {
        b"*-1\r\n".to_vec()
    }
}

pub fn array_len(n: i64) -> Vec<u8> {
    format!("*{n}\r\n").into_bytes()
}

pub fn map_len(proto: i32, n: i64) -> Vec<u8> {
    if proto >= 3 {
        format!("%{n}\r\n").into_bytes()
    } else {
        format!("*{}\r\n", n * 2).into_bytes()
    }
}

pub fn push_len(proto: i32, n: i64) -> Vec<u8> {
    if proto >= 3 {
        format!(">{n}\r\n").into_bytes()
    } else {
        format!("*{n}\r\n").into_bytes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn split_array_and_nulls() {
        let full = b"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
        assert!(matches!(parse(&full[..8]), Parse::NeedMore));
        match parse(full) {
            Parse::Done { value, consumed } => {
                assert_eq!(consumed, full.len());
                match value {
                    Value::Array(items) => {
                        assert_eq!(items[0], Value::Bulk(b"GET".to_vec()));
                        assert_eq!(items[1], Value::Bulk(b"key".to_vec()));
                    }
                    other => panic!("{other:?}"),
                }
            }
            other => panic!("{other:?}"),
        }
        assert!(matches!(parse(b"$5\r\nab"), Parse::NeedMore));
        assert!(matches!(parse(b"$1\r\nX\n\n"), Parse::Error));
    }
}
