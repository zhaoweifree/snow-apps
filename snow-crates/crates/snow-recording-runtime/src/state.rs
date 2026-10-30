#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecordingState {
    Created,
    Running,
    Paused,
    Stopped,
}

#[cfg(feature = "legacy-capture")]
impl RecordingState {
    pub(crate) fn as_u8(self) -> u8 {
        match self {
            Self::Created => 0,
            Self::Running => 1,
            Self::Paused => 2,
            Self::Stopped => 3,
        }
    }

    pub(crate) fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Running,
            2 => Self::Paused,
            3 => Self::Stopped,
            _ => Self::Created,
        }
    }
}
