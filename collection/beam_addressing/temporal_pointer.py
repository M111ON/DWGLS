#!/usr/bin/env python3
"""
Temporal Pointer System
=======================
Pointer tied to time → direct start<>end access
No intermediate path storage needed

Pointer = (angle, length, time_step)
Point to any time → get position instantly
"""

import numpy as np
import time
from dataclasses import dataclass
from typing import Dict, List, Tuple, Optional

# ============================================================================
# CORE: Temporal Pointer
# ============================================================================

@dataclass
class BeamPointer:
    """Beam pointer with time dimension"""
    angle: int = 0
    length: int = 0
    time_step: int = 0
    
    def move(self, angle: int, length: int, time_step: int):
        """Move pointer to new position"""
        self.angle = angle
        self.length = length
        self.time_step = time_step
    
    def to_tuple(self) -> Tuple[int, int, int]:
        return (self.angle, self.length, self.time_step)

@dataclass 
class DataRecord:
    """Data with entry and settle points (no intermediate path)"""
    data_id: int
    value: float
    
    # Entry point
    entry_time: int
    entry_angle: int
    entry_length: int
    
    # Settle point
    settle_time: int
    settle_angle: int
    settle_length: int
    
    def get_entry_position(self) -> Tuple[int, int]:
        return (self.entry_angle, self.entry_length)
    
    def get_settle_position(self) -> Tuple[int, int]:
        return (self.settle_angle, self.settle_length)
    
    def duration(self) -> int:
        return self.settle_time - self.entry_time

# ============================================================================
# TEMPORAL FIELD (Pointer-based Access)
# ============================================================================

class TemporalField:
    """
    Field where pointer ties to time.
    Point to any time step → get data position instantly.
    """
    
    def __init__(self):
        # Time-indexed storage: time_step → list of (position, data_id)
        self.time_index: Dict[int, List[Tuple[Tuple[int, int], int]]] = {}
        
        # Data records: data_id → DataRecord
        self.records: Dict[int, DataRecord] = {}
        
        # Active tracking: data_id → current position
        self.active: Dict[int, Tuple[int, int, int]] = {}  # (angle, length, time)
        
        # Pointer
        self.pointer = BeamPointer()
        
        # Stats
        self.total_data = 0
        self.current_time = 0
        
        print("[TemporalField] Initialized")
    
    def data_enter(self, data_id: int, value: float, angle: int, length: int):
        """Data enters the field at current time"""
        # Store entry position separately (don't overwrite during moves)
        self.active[data_id] = {
            'current': (angle, length, self.current_time),
            'entry': (angle, length, self.current_time)
        }
        
        print(f"[Enter] Data {data_id} at ({angle}, {length}) time={self.current_time}")
    
    def data_move(self, data_id: int, new_angle: int, new_length: int):
        """Data moves to new position (updates active tracking, preserves entry)"""
        if data_id in self.active:
            entry = self.active[data_id]['entry']
            self.active[data_id] = {
                'current': (new_angle, new_length, entry[2]),
                'entry': entry  # Preserve original entry
            }
    
    def data_settle(self, data_id: int) -> Optional[DataRecord]:
        """Data settles (stops moving) - creates record"""
        if data_id not in self.active:
            return None
        
        # Get current position and preserved entry position
        current_angle, current_length, entry_time = self.active[data_id]['current']
        entry_angle, entry_length, _ = self.active[data_id]['entry']
        
        # Create record with correct entry and settle positions
        record = DataRecord(
            data_id=data_id,
            value=float(data_id),
            entry_time=entry_time,
            entry_angle=entry_angle,
            entry_length=entry_length,
            settle_time=self.current_time,
            settle_angle=current_angle,
            settle_length=current_length
        )
        
        self.records[data_id] = record
        
        # Add to time index
        if entry_time not in self.time_index:
            self.time_index[entry_time] = []
        self.time_index[entry_time].append(((entry_angle, entry_length), data_id))
        
        if self.current_time not in self.time_index:
            self.time_index[self.current_time] = []
        self.time_index[self.current_time].append(((current_angle, current_length), data_id))
        
        # Remove from active
        del self.active[data_id]
        
        print(f"[Settle] Data {data_id}: ({entry_angle}, {entry_length}) → ({current_angle}, {current_length}) time={entry_time}→{self.current_time}")
        
        return record
    
    def tick(self):
        """Advance time by one step"""
        self.current_time += 1
    
    # ========================================================================
    # POINTER OPERATIONS (The Key Feature)
    # ========================================================================
    
    def pointer_move(self, angle: int, length: int, time_step: int):
        """Move pointer to specific time"""
        self.pointer.move(angle, length, time_step)
    
    def pointer_get_entry(self, data_id: int) -> Optional[Tuple[int, int, int]]:
        """Point to data's entry position (instant access)"""
        record = self.records.get(data_id)
        if record:
            return (record.entry_angle, record.entry_length, record.entry_time)
        return None
    
    def pointer_get_settle(self, data_id: int) -> Optional[Tuple[int, int, int]]:
        """Point to data's settle position (instant access)"""
        record = self.records.get(data_id)
        if record:
            return (record.settle_angle, record.settle_length, record.settle_time)
        return None
    
    def pointer_get_position_at_time(self, data_id: int, time_step: int) -> Optional[Tuple[int, int]]:
        """
        Get data position at specific time.
        If time == entry_time → return entry position
        If time == settle_time → return settle position
        Otherwise → interpolate (or return None)
        """
        record = self.records.get(data_id)
        if not record:
            return None
        
        # Direct access at entry or settle time
        if time_step == record.entry_time:
            return (record.entry_angle, record.entry_length)
        elif time_step == record.settle_time:
            return (record.settle_angle, record.settle_length)
        else:
            # For intermediate times, we could interpolate
            # But per user request: just return None (don't care about intermediates)
            return None
    
    def pointer_scan_time_range(self, time_start: int, time_end: int) -> List[DataRecord]:
        """Get all data records that were active in time range"""
        results = []
        for record in self.records.values():
            if record.entry_time <= time_end and record.settle_time >= time_start:
                results.append(record)
        return results
    
    def get_direct_start_end(self, data_id: int) -> Optional[Dict]:
        """
        DIRECT START<>END ACCESS
        Point to data_id → get entry and settle immediately
        """
        record = self.records.get(data_id)
        if not record:
            return None
        
        return {
            'data_id': data_id,
            'entry': {
                'position': (record.entry_angle, record.entry_length),
                'time': record.entry_time
            },
            'settle': {
                'position': (record.settle_angle, record.settle_length),
                'time': record.settle_time
            },
            'duration': record.duration()
        }
    
    def print_stats(self):
        """Print field statistics"""
        print(f"\n{'='*60}")
        print(f"TEMPORAL FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Current time:      {self.current_time}")
        print(f"Total data:        {self.total_data}")
        print(f"Completed:         {len(self.records)}")
        print(f"Active now:        {len(self.active)}")
        print(f"Time index entries: {len(self.time_index)}")
        print(f"Pointer position:  {self.pointer.to_tuple()}")
        print(f"{'='*60}\n")


# ============================================================================
# DEMO
# ============================================================================

def demo_temporal_pointer():
    """Demonstrate temporal pointer system"""
    print("\n" + "="*60)
    print("TEMPORAL POINTER SYSTEM DEMO")
    print("="*60)
    
    field = TemporalField()
    
    # Simulate data entering and settling over time
    print("\n[Sim] Simulating data flow...")
    
    # Time 0: Data 1, 2, 3 enter
    field.tick()
    field.data_enter(1, 10.0, angle=10, length=5)
    field.data_enter(2, 20.0, angle=50, length=3)
    field.data_enter(3, 30.0, angle=90, length=7)
    
    # Time 1: Data moves
    field.tick()
    field.data_move(1, new_angle=15, new_length=6)
    field.data_move(2, new_angle=45, new_length=4)
    field.data_move(3, new_angle=85, new_length=8)
    
    # Time 2: Data 1 settles
    field.tick()
    field.data_move(1, new_angle=20, new_length=7)
    field.data_settle(1)
    
    # Time 3: Data 2, 3 continue
    field.tick()
    field.data_move(2, new_angle=40, new_length=5)
    field.data_move(3, new_angle=80, new_length=9)
    
    # Time 4: Data 2 settles
    field.tick()
    field.data_settle(2)
    
    # Time 5: Data 3 settles
    field.tick()
    field.data_move(3, new_angle=75, new_length=10)
    field.data_settle(3)
    
    field.print_stats()
    
    # ========================================================================
    # POINTER OPERATIONS DEMO
    # ========================================================================
    
    print("\n" + "="*60)
    print("POINTER OPERATIONS")
    print("="*60)
    
    # Direct start<>end access
    print("\n[Pointer] Direct START<>END access:")
    for data_id in [1, 2, 3]:
        result = field.get_direct_start_end(data_id)
        if result:
            print(f"\n  Data {data_id}:")
            print(f"    Entry:  {result['entry']['position']} at time {result['entry']['time']}")
            print(f"    Settle: {result['settle']['position']} at time {result['settle']['time']}")
            print(f"    Duration: {result['duration']} ticks")
    
    # Point to specific time
    print("\n[Pointer] Point to specific times:")
    field.pointer_move(angle=0, length=0, time_step=2)
    print(f"  Pointer at time {field.pointer.time_step}")
    
    for data_id in [1, 2, 3]:
        pos = field.pointer_get_position_at_time(data_id, field.pointer.time_step)
        if pos:
            print(f"    Data {data_id} at time {field.pointer.time_step}: {pos}")
        else:
            print(f"    Data {data_id} at time {field.pointer.time_step}: (not at entry/settle)")
    
    # Scan time range
    print("\n[Pointer] Scan time range [1, 4]:")
    records = field.pointer_scan_time_range(1, 4)
    for rec in records:
        print(f"  Data {rec.data_id}: ({rec.entry_angle},{rec.entry_length}) → ({rec.settle_angle},{rec.settle_length})")
    
    print("\n" + "="*60)
    print("DEMO COMPLETE")
    print("="*60)


if __name__ == "__main__":
    demo_temporal_pointer()
