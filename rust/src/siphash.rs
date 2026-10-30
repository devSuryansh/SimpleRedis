#[allow(dead_code)]
pub fn siphash24(key: &[u8; 16], data: &[u8]) -> u64 {
    fn rotl(x: u64, b: u32) -> u64 {
        x.rotate_left(b)
    }
    fn read64(p: &[u8]) -> u64 {
        let mut v = 0u64;
        for (i, byte) in p.iter().take(8).enumerate() {
            v |= u64::from(*byte) << (8 * i);
        }
        v
    }
    fn round(v: &mut [u64; 4]) {
        v[0] = v[0].wrapping_add(v[1]);
        v[1] = rotl(v[1], 13) ^ v[0];
        v[0] = rotl(v[0], 32);
        v[2] = v[2].wrapping_add(v[3]);
        v[3] = rotl(v[3], 16) ^ v[2];
        v[0] = v[0].wrapping_add(v[3]);
        v[3] = rotl(v[3], 21) ^ v[0];
        v[2] = v[2].wrapping_add(v[1]);
        v[1] = rotl(v[1], 17) ^ v[2];
        v[2] = rotl(v[2], 32);
    }

    let k0 = read64(&key[..8]);
    let k1 = read64(&key[8..]);
    let mut v = [
        0x736f_6d65_7073_6575 ^ k0,
        0x646f_7261_6e64_6f6d ^ k1,
        0x6c79_6765_6e65_7261 ^ k0,
        0x7465_6462_7974_6573 ^ k1,
    ];
    let mut b = (data.len() as u64) << 56;
    let mut chunks = data.chunks_exact(8);
    for block in chunks.by_ref() {
        let m = read64(block);
        v[3] ^= m;
        round(&mut v);
        round(&mut v);
        v[0] ^= m;
    }
    let rest = chunks.remainder();
    for (i, byte) in rest.iter().enumerate() {
        b |= u64::from(*byte) << (8 * i);
    }
    v[3] ^= b;
    round(&mut v);
    round(&mut v);
    v[0] ^= b;
    v[2] ^= 0xff;
    round(&mut v);
    round(&mut v);
    round(&mut v);
    round(&mut v);
    v[0] ^ v[1] ^ v[2] ^ v[3]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn official_vector() {
        let key: [u8; 16] = core::array::from_fn(|i| i as u8);
        let msg: Vec<u8> = (0..15).map(|i| i as u8).collect();
        assert_eq!(siphash24(&key, &msg), 0xa129_ca61_49be_45e5);
    }
}
