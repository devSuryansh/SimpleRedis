#[derive(Clone, Debug)]
struct Level {
    forward: Option<usize>,
    span: usize,
}

#[derive(Clone, Debug)]
struct Node {
    member: Vec<u8>,
    score: f64,
    backward: Option<usize>,
    levels: Vec<Level>,
}

#[derive(Clone)]
pub struct Skiplist {
    nodes: Vec<Node>,
    level: usize,
    length: usize,
}

impl Skiplist {
    pub fn new() -> Self {
        let header = Node {
            member: Vec::new(),
            score: 0.0,
            backward: None,
            levels: (0..32)
                .map(|_| Level {
                    forward: None,
                    span: 0,
                })
                .collect(),
        };
        Self {
            nodes: vec![header],
            level: 1,
            length: 0,
        }
    }

    fn random_level() -> usize {
        let mut level = 1;
        while level < 32 && (rand() & 0xffff) < (0.25 * 65535.0) as u32 {
            level += 1;
        }
        level
    }

    fn less(score: f64, member: &[u8], other_score: f64, other_member: &[u8]) -> bool {
        score < other_score || (score == other_score && member < other_member)
    }

    pub fn insert(&mut self, score: f64, member: Vec<u8>) {
        let mut update = [0usize; 32];
        let mut rank = [0usize; 32];
        let mut x = 0usize;
        for i in (0..self.level).rev() {
            rank[i] = if i + 1 == self.level { 0 } else { rank[i + 1] };
            while let Some(fwd) = self.nodes[x].levels[i].forward {
                let node = &self.nodes[fwd];
                if Self::less(node.score, &node.member, score, &member) {
                    rank[i] += self.nodes[x].levels[i].span;
                    x = fwd;
                } else {
                    break;
                }
            }
            update[i] = x;
        }
        let new_level = Self::random_level();
        if new_level > self.level {
            for i in self.level..new_level {
                rank[i] = 0;
                update[i] = 0;
                self.nodes[0].levels[i].span = self.length;
            }
            self.level = new_level;
        }
        let idx = self.nodes.len();
        self.nodes.push(Node {
            member,
            score,
            backward: None,
            levels: (0..new_level)
                .map(|_| Level {
                    forward: None,
                    span: 0,
                })
                .collect(),
        });
        for i in 0..new_level {
            let forward = self.nodes[update[i]].levels[i].forward;
            self.nodes[idx].levels[i].forward = forward;
            self.nodes[update[i]].levels[i].forward = Some(idx);
            let span = self.nodes[update[i]].levels[i].span;
            self.nodes[idx].levels[i].span = span.saturating_sub(rank[0] - rank[i]);
            self.nodes[update[i]].levels[i].span = rank[0] - rank[i] + 1;
        }
        for i in new_level..self.level {
            self.nodes[update[i]].levels[i].span += 1;
        }
        self.nodes[idx].backward = if update[0] == 0 { None } else { Some(update[0]) };
        if let Some(fwd) = self.nodes[idx].levels[0].forward {
            self.nodes[fwd].backward = Some(idx);
        }
        self.length += 1;
    }

    pub fn delete(&mut self, score: f64, member: &[u8]) -> bool {
        let mut update = [0usize; 32];
        let mut x = 0usize;
        for i in (0..self.level).rev() {
            while let Some(fwd) = self.nodes[x].levels[i].forward {
                let node = &self.nodes[fwd];
                if Self::less(node.score, &node.member, score, member) {
                    x = fwd;
                } else {
                    break;
                }
            }
            update[i] = x;
        }
        let Some(target) = self.nodes[x].levels[0].forward else {
            return false;
        };
        if self.nodes[target].score != score || self.nodes[target].member != member {
            return false;
        }
        for i in 0..self.level {
            if self.nodes[update[i]].levels[i].forward == Some(target) {
                let span = self.nodes[target].levels.get(i).map(|l| l.span).unwrap_or(0);
                let forward = self.nodes[target].levels.get(i).and_then(|l| l.forward);
                self.nodes[update[i]].levels[i].span =
                    self.nodes[update[i]].levels[i].span + span.saturating_sub(1);
                self.nodes[update[i]].levels[i].forward = forward;
            } else {
                self.nodes[update[i]].levels[i].span =
                    self.nodes[update[i]].levels[i].span.saturating_sub(1);
            }
        }
        if let Some(fwd) = self.nodes[target].levels[0].forward {
            self.nodes[fwd].backward = self.nodes[target].backward;
        }
        while self.level > 1 && self.nodes[0].levels[self.level - 1].forward.is_none() {
            self.level -= 1;
        }
        self.length -= 1;
        self.nodes[target].levels.clear();
        self.nodes[target].member.clear();
        true
    }

    pub fn rank(&self, score: f64, member: &[u8]) -> Option<usize> {
        let mut x = 0usize;
        let mut rank = 0usize;
        for i in (0..self.level).rev() {
            while let Some(fwd) = self.nodes[x].levels[i].forward {
                let node = &self.nodes[fwd];
                if node.score < score || (node.score == score && node.member.as_slice() <= member) {
                    rank += self.nodes[x].levels[i].span;
                    x = fwd;
                } else {
                    break;
                }
            }
            if x != 0 && self.nodes[x].member == member {
                return Some(rank);
            }
        }
        None
    }

    pub fn by_rank(&self, rank: usize) -> Option<(&[u8], f64)> {
        if rank == 0 || rank > self.length {
            return None;
        }
        let mut x = 0usize;
        let mut traversed = 0usize;
        for i in (0..self.level).rev() {
            while let Some(fwd) = self.nodes[x].levels[i].forward {
                if traversed + self.nodes[x].levels[i].span <= rank {
                    traversed += self.nodes[x].levels[i].span;
                    x = fwd;
                } else {
                    break;
                }
            }
            if traversed == rank {
                let node = &self.nodes[x];
                return Some((&node.member, node.score));
            }
        }
        None
    }

    pub fn len(&self) -> usize {
        self.length
    }
}

fn rand() -> u32 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static STATE: AtomicU64 = AtomicU64::new(0x1234_5678_9abc_def0);
    let mut x = STATE.load(Ordering::Relaxed);
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    STATE.store(x, Ordering::Relaxed);
    x as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_scores_order_by_member() {
        let mut zsl = Skiplist::new();
        zsl.insert(1.0, b"b".to_vec());
        zsl.insert(1.0, b"a".to_vec());
        zsl.insert(3.0, b"c".to_vec());
        assert_eq!(zsl.rank(1.0, b"a"), Some(1));
        assert_eq!(zsl.rank(1.0, b"b"), Some(2));
        assert_eq!(zsl.rank(3.0, b"c"), Some(3));
        assert!(zsl.delete(1.0, b"b"));
        assert_eq!(zsl.rank(3.0, b"c"), Some(2));
        assert_eq!(zsl.by_rank(1).map(|(m, _)| m.to_vec()), Some(b"a".to_vec()));
    }
}
