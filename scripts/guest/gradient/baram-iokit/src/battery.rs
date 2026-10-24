#[derive(Debug, Clone, Copy, PartialEq)]
pub enum BatteryStatus {
    Discharging,
    Charging,
    Full,
    NotPresent,
    Unknown,
}

pub struct BatteryInfo {
    pub percentage: u8,
    pub status: BatteryStatus,
}

impl Default for BatteryInfo {
    fn default() -> Self {
        Self {
            percentage: 0,
            status: BatteryStatus::Unknown,
        }
    }
}

impl BatteryInfo {
    pub fn valid_percentage(&self) -> Option<u8> {
        match self.status {
            BatteryStatus::Discharging | BatteryStatus::Charging | BatteryStatus::Full
                if self.percentage <= 100 =>
            {
                Some(self.percentage)
            }
            _ => None,
        }
    }
}

pub fn read_battery() -> BatteryInfo {
    for name in ["BAT0", "BAT1", "battery"] {
        let base = format!("/sys/class/power_supply/{name}");
        let Ok(capacity) = std::fs::read_to_string(format!("{base}/capacity")) else {
            continue;
        };
        let Ok(percentage) = capacity.trim().parse::<u8>() else {
            continue;
        };
        let status = std::fs::read_to_string(format!("{base}/status"))
            .ok()
            .map(|value| match value.trim() {
                "Charging" => BatteryStatus::Charging,
                "Full" => BatteryStatus::Full,
                "Discharging" => BatteryStatus::Discharging,
                "Not present" => BatteryStatus::NotPresent,
                _ => BatteryStatus::Unknown,
            })
            .unwrap_or(BatteryStatus::Unknown);
        return BatteryInfo {
            percentage,
            status,
        };
    }
    BatteryInfo::default()
}
