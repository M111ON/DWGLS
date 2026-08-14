#!/usr/bin/env python3
"""
Observation Field + Blueprint System
=====================================
Passive field tracks data movement → extracts patterns → stores blueprints
Reconstruction: entry + settle + blueprint_id + tick_steps → full trajectory
"""

import numpy as np
import json
import time
from dataclasses import dataclass, field
from typing import Dict, List, Tuple, Optional
from collections import defaultdict
from enum import Enum

# ============================================================================
# CORE DATA STRUCTURES
# ============================================================================

class PatternType(Enum):
    LINEAR = "linear"
    ANGULAR = "angular"  
    GRAVITY = "gravity"
    RANDOM = "random"
    SPIRAL = "spiral"
    BOUNCE = "bounce"

@dataclass
class BeamCoordinate:
    angle: int
    length: int
    step: int
    
    def to_tuple(self) -> Tuple[int, int, int]:
        return (self.angle, self.length, self.step)
    
    def distance_to(self, other: 'BeamCoordinate') -> float:
        """Euclidean distance in beam space"""
        return np.sqrt(
            (self.angle - other.angle)**2 +
            (self.length - other.length)**2 +
            (self.step - other.step)**2
        )
    
    def __hash__(self):
        return hash((self.angle, self.length, self.step))

@dataclass
class Trajectory:
    """Complete data movement path"""
    entry: BeamCoordinate
    settle: BeamCoordinate
    path: List[BeamCoordinate]
    tick_steps: int
    value: float
    trajectory_id: int = 0

@dataclass 
class Blueprint:
    """Representative trajectory pattern"""
    id: int
    pattern_type: PatternType
    parameters: Dict  # pattern-specific params
    canonical_path: List[BeamCoordinate]  # representative example
    frequency: int = 0  # how often this pattern occurs

@dataclass
class CompressedRecord:
    """Compressed storage of a trajectory"""
    entry: BeamCoordinate
    settle: BeamCoordinate
    blueprint_id: int
    tick_steps: int
    value: float


# ============================================================================
# OBSERVATION FIELD (Passive Tracker)
# ============================================================================

class ObservationField:
    """
    Passive field that observes and records all data movement.
    Does not influence data - just watches.
    """
    
    def __init__(self):
        # Active tracking state
        self.active_trajectories: Dict[float, List[BeamCoordinate]] = {}
        
        # Completed trajectories
        self.completed_trajectories: List[Trajectory] = []
        
        # Statistics
        self.total_entries = 0
        self.total_movements = 0
        self.total_settles = 0
        
        print("[ObservationField] Initialized - passive tracking mode")
    
    def record_entry(self, value: float, coordinate: BeamCoordinate):
        """Record data entering the field"""
        self.active_trajectories[value] = [coordinate]
        self.total_entries += 1
    
    def record_movement(self, value: float, new_coordinate: BeamCoordinate):
        """Record data moving to new position"""
        if value in self.active_trajectories:
            self.active_trajectories[value].append(new_coordinate)
            self.total_movements += 1
    
    def record_settle(self, value: float, coordinate: BeamCoordinate) -> Optional[Trajectory]:
        """Record data settling (stopping) - returns completed trajectory"""
        if value not in self.active_trajectories:
            return None
        
        path = self.active_trajectories[value]
        path.append(coordinate)
        
        # Create trajectory record
        trajectory = Trajectory(
            entry=path[0],
            settle=path[-1],
            path=path,
            tick_steps=len(path) - 1,
            value=value,
            trajectory_id=len(self.completed_trajectories)
        )
        
        self.completed_trajectories.append(trajectory)
        del self.active_trajectories[value]
        self.total_settles += 1
        
        return trajectory
    
    def get_trajectories(self) -> List[Trajectory]:
        """Get all completed trajectories"""
        return self.completed_trajectories
    
    def get_active_count(self) -> int:
        """Number of data points still moving"""
        return len(self.active_trajectories)
    
    def print_stats(self):
        """Print field statistics"""
        print(f"\n{'='*60}")
        print(f"OBSERVATION FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Total entries:      {self.total_entries}")
        print(f"Total movements:    {self.total_movements}")
        print(f"Total settles:      {self.total_settles}")
        print(f"Active now:         {self.get_active_count()}")
        print(f"Completed trajs:    {len(self.completed_trajectories)}")
        print(f"{'='*60}\n")


# ============================================================================
# BLUEPRINT ENGINE (Pattern Extraction)
# ============================================================================

class BlueprintEngine:
    """
    Analyzes trajectories and extracts common patterns.
    Creates blueprints for reconstruction.
    """
    
    def __init__(self):
        self.blueprints: Dict[int, Blueprint] = {}
        self.next_blueprint_id = 0
        
        # Trajectory classification cache
        self.classification_cache: Dict[int, int] = {}  # traj_id → blueprint_id
        
        print("[BlueprintEngine] Initialized")
    
    def classify_trajectory(self, traj: Trajectory) -> PatternType:
        """Classify trajectory into pattern type based on movement characteristics"""
        if len(traj.path) < 2:
            return PatternType.LINEAR
        
        # Calculate movement vectors
        deltas = []
        for i in range(1, len(traj.path)):
            dx = traj.path[i].angle - traj.path[i-1].angle
            dy = traj.path[i].length - traj.path[i-1].length
            dz = traj.path[i].step - traj.path[i-1].step
            deltas.append((dx, dy, dz))
        
        if not deltas:
            return PatternType.LINEAR
        
        # Check for linear movement (constant direction)
        angles = [np.arctan2(dy, dx) for dx, dy, dz in deltas if dx != 0 or dy != 0]
        if angles and np.std(angles) < 0.1:  # Low variance = linear
            return PatternType.LINEAR
        
        # Check for angular movement (rotation around center)
        distances = [traj.entry.distance_to(p) for p in traj.path]
        if len(distances) > 2:
            dist_variance = np.var(distances)
            if dist_variance < 5.0:  # Relatively constant distance = rotation
                return PatternType.ANGULAR
        
        # Check for gravity (increasing speed downward)
        speeds = [np.sqrt(dx**2 + dy**2 + dz**2) for dx, dy, dz in deltas]
        if len(speeds) > 2:
            speed_trend = np.polyfit(range(len(speeds)), speeds, 1)[0]
            if speed_trend > 0.1:  # Accelerating = gravity
                return PatternType.GRAVITY
        
        # Check for spiral (changing radius + rotation)
        if len(distances) > 3:
            dist_change = np.diff(distances)
            if np.any(dist_change > 0) and np.any(dist_change < 0):
                return PatternType.SPIRAL
        
        # Check for bounce (direction reversal)
        direction_changes = 0
        for i in range(2, len(deltas)):
            prev_dir = np.sign(deltas[i-1][1])  # Y direction
            curr_dir = np.sign(deltas[i][1])
            if prev_dir != curr_dir and prev_dir != 0:
                direction_changes += 1
        
        if direction_changes >= 2:
            return PatternType.BOUNCE
        
        return PatternType.RANDOM
    
    def extract_parameters(self, traj: Trajectory, pattern_type: PatternType) -> Dict:
        """Extract pattern-specific parameters"""
        params = {}
        
        if pattern_type == PatternType.LINEAR:
            # Calculate slope and speed
            if len(traj.path) >= 2:
                dx = traj.settle.angle - traj.entry.angle
                dy = traj.settle.length - traj.entry.length
                params['slope'] = dy / max(dx, 1)
                params['speed'] = traj.entry.distance_to(traj.settle) / max(traj.tick_steps, 1)
        
        elif pattern_type == PatternType.ANGULAR:
            # Calculate radius and angular velocity
            distances = [traj.entry.distance_to(p) for p in traj.path]
            params['radius'] = np.mean(distances)
            params['angular_velocity'] = len(traj.path) / max(traj.tick_steps, 1)
        
        elif pattern_type == PatternType.GRAVITY:
            # Calculate acceleration
            speeds = []
            for i in range(1, len(traj.path)):
                d = traj.path[i-1].distance_to(traj.path[i])
                speeds.append(d)
            if speeds:
                params['acceleration'] = np.polyfit(range(len(speeds)), speeds, 1)[0]
                params['initial_speed'] = speeds[0] if speeds else 0
        
        elif pattern_type == PatternType.SPIRAL:
            # Calculate spiral parameters
            distances = [traj.entry.distance_to(p) for p in traj.path]
            params['initial_radius'] = distances[0] if distances else 0
            params['final_radius'] = distances[-1] if distances else 0
            params['turns'] = len(traj.path) / 10  # Approximate
        
        elif pattern_type == PatternType.BOUNCE:
            # Calculate bounce count and energy loss
            bounce_count = 0
            for i in range(2, len(traj.path)):
                prev_dy = traj.path[i-1].length - traj.path[i-2].length
                curr_dy = traj.path[i].length - traj.path[i-1].length
                if prev_dy * curr_dy < 0:
                    bounce_count += 1
            params['bounce_count'] = bounce_count
        
        return params
    
    def find_similar_blueprint(self, traj: Trajectory, pattern_type: PatternType) -> Optional[int]:
        """Find existing blueprint that matches this trajectory"""
        for bp_id, bp in self.blueprints.items():
            if bp.pattern_type != pattern_type:
                continue
            
            # Compare trajectory shape (normalized)
            # Simple comparison: entry-settle vector similarity
            traj_vector = np.array([
                traj.settle.angle - traj.entry.angle,
                traj.settle.length - traj.entry.length,
                traj.settle.step - traj.entry.step
            ])
            
            bp_vector = np.array([
                bp.canonical_path[-1].angle - bp.canonical_path[0].angle,
                bp.canonical_path[-1].length - bp.canonical_path[0].length,
                bp.canonical_path[-1].step - bp.canonical_path[0].step
            ])
            
            # Normalize and compare
            traj_norm = traj_vector / (np.linalg.norm(traj_vector) + 1e-8)
            bp_norm = bp_vector / (np.linalg.norm(bp_vector) + 1e-8)
            
            similarity = np.dot(traj_norm, bp_norm)
            
            if similarity > 0.8:  # 80% similar
                return bp_id
        
        return None
    
    def build_blueprints(self, trajectories: List[Trajectory], min_frequency: int = 2):
        """Analyze all trajectories and build blueprint database"""
        print(f"\n[BlueprintEngine] Analyzing {len(trajectories)} trajectories...")
        
        # Classify all trajectories
        pattern_groups: Dict[PatternType, List[Trajectory]] = defaultdict(list)
        
        for traj in trajectories:
            pattern_type = self.classify_trajectory(traj)
            pattern_groups[pattern_type].append(traj)
        
        # Create blueprints for each pattern type
        for pattern_type, trajs in pattern_groups.items():
            print(f"  {pattern_type.value}: {len(trajs)} trajectories")
            
            # Use first trajectory as canonical example
            canonical = trajs[0]
            
            # Extract parameters (use median values if multiple)
            if len(trajs) > 1:
                # TODO: average parameters across similar trajectories
                params = self.extract_parameters(canonical, pattern_type)
            else:
                params = self.extract_parameters(canonical, pattern_type)
            
            # Create blueprint
            blueprint = Blueprint(
                id=self.next_blueprint_id,
                pattern_type=pattern_type,
                parameters=params,
                canonical_path=canonical.path,
                frequency=len(trajs)
            )
            
            self.blueprints[self.next_blueprint_id] = blueprint
            
            # Cache classifications
            for traj in trajs:
                self.classification_cache[traj.trajectory_id] = self.next_blueprint_id
            
            self.next_blueprint_id += 1
        
        print(f"[BlueprintEngine] Created {len(self.blueprints)} blueprints")
    
    def compress_trajectory(self, traj: Trajectory) -> CompressedRecord:
        """Compress a trajectory using blueprints"""
        # Get blueprint for this trajectory
        blueprint_id = self.classification_cache.get(traj.trajectory_id, 0)
        
        return CompressedRecord(
            entry=traj.entry,
            settle=traj.settle,
            blueprint_id=blueprint_id,
            tick_steps=traj.tick_steps,
            value=traj.value
        )
    
    def print_blueprints(self):
        """Print blueprint database"""
        print(f"\n{'='*60}")
        print(f"BLUEPRINT DATABASE")
        print(f"{'='*60}")
        
        for bp_id, bp in self.blueprints.items():
            print(f"\nBlueprint {bp_id}: {bp.pattern_type.value}")
            print(f"  Frequency: {bp.frequency}")
            print(f"  Parameters: {bp.parameters}")
            print(f"  Path length: {len(bp.canonical_path)} points")
        
        print(f"\n{'='*60}\n")


# ============================================================================
# RECONSTRUCTOR (Blueprint-based Reconstruction)
# ============================================================================

class Reconstructor:
    """
    Rebuilds full trajectory from compressed record.
    Uses blueprint patterns to interpolate path.
    """
    
    def __init__(self, blueprint_engine: BlueprintEngine):
        self.engine = blueprint_engine
        print("[Reconstructor] Initialized")
    
    def reconstruct(self, record: CompressedRecord) -> List[BeamCoordinate]:
        """Reconstruct full trajectory from compressed record"""
        
        # Get blueprint
        blueprint = self.engine.blueprints.get(record.blueprint_id)
        if not blueprint:
            # Fallback: linear interpolation
            return self._linear_interpolate(record.entry, record.settle, record.tick_steps)
        
        # Reconstruct based on pattern type
        if blueprint.pattern_type == PatternType.LINEAR:
            return self._reconstruct_linear(record, blueprint)
        elif blueprint.pattern_type == PatternType.ANGULAR:
            return self._reconstruct_angular(record, blueprint)
        elif blueprint.pattern_type == PatternType.GRAVITY:
            return self._reconstruct_gravity(record, blueprint)
        elif blueprint.pattern_type == PatternType.SPIRAL:
            return self._reconstruct_spiral(record, blueprint)
        elif blueprint.pattern_type == PatternType.BOUNCE:
            return self._reconstruct_bounce(record, blueprint)
        else:
            return self._linear_interpolate(record.entry, record.settle, record.tick_steps)
    
    def _linear_interpolate(self, start: BeamCoordinate, end: BeamCoordinate, 
                           num_steps: int) -> List[BeamCoordinate]:
        """Simple linear interpolation between two points"""
        path = []
        for i in range(num_steps + 1):
            t = i / max(num_steps, 1)
            angle = int(start.angle + (end.angle - start.angle) * t)
            length = int(start.length + (end.length - start.length) * t)
            step = int(start.step + (end.step - start.step) * t)
            path.append(BeamCoordinate(angle, length, step))
        return path
    
    def _reconstruct_linear(self, record: CompressedRecord, blueprint: Blueprint) -> List[BeamCoordinate]:
        """Reconstruct linear trajectory"""
        # Use blueprint speed with entry-settle direction
        speed = blueprint.parameters.get('speed', 1.0)
        
        path = []
        current = record.entry
        
        for i in range(record.tick_steps + 1):
            path.append(BeamCoordinate(current.angle, current.length, current.step))
            
            # Calculate next position
            if i < record.tick_steps:
                t = 1.0 / record.tick_steps
                dx = (record.settle.angle - record.entry.angle) * t
                dy = (record.settle.length - record.entry.length) * t
                
                current = BeamCoordinate(
                    int(current.angle + dx),
                    int(current.length + dy),
                    current.step + 1
                )
        
        return path
    
    def _reconstruct_angular(self, record: CompressedRecord, blueprint: Blueprint) -> List[BeamCoordinate]:
        """Reconstruct angular (rotational) trajectory"""
        radius = blueprint.parameters.get('radius', 5.0)
        angular_velocity = blueprint.parameters.get('angular_velocity', 0.5)
        
        # Center point (midpoint between entry and settle)
        center_angle = (record.entry.angle + record.settle.angle) // 2
        center_length = (record.entry.length + record.settle.length) // 2
        
        path = []
        for i in range(record.tick_steps + 1):
            angle = record.entry.angle + int(radius * np.cos(i * angular_velocity))
            length = record.entry.length + int(radius * np.sin(i * angular_velocity))
            step = record.entry.step + i
            
            path.append(BeamCoordinate(angle % 360, length, step))
        
        return path
    
    def _reconstruct_gravity(self, record: CompressedRecord, blueprint: Blueprint) -> List[BeamCoordinate]:
        """Reconstruct gravity (accelerating) trajectory"""
        acceleration = blueprint.parameters.get('acceleration', 0.5)
        initial_speed = blueprint.parameters.get('initial_speed', 1.0)
        
        path = []
        current_angle = record.entry.angle
        current_length = record.entry.length
        
        for i in range(record.tick_steps + 1):
            path.append(BeamCoordinate(current_angle, current_length, record.entry.step + i))
            
            # Apply acceleration
            speed = initial_speed + acceleration * i
            current_length += int(speed)
            current_angle += int((record.settle.angle - record.entry.angle) / record.tick_steps)
        
        return path
    
    def _reconstruct_spiral(self, record: CompressedRecord, blueprint: Blueprint) -> List[BeamCoordinate]:
        """Reconstruct spiral trajectory"""
        initial_radius = blueprint.parameters.get('initial_radius', 5.0)
        final_radius = blueprint.parameters.get('final_radius', 15.0)
        turns = blueprint.parameters.get('turns', 2.0)
        
        center_angle = (record.entry.angle + record.settle.angle) // 2
        center_length = (record.entry.length + record.settle.length) // 2
        
        path = []
        for i in range(record.tick_steps + 1):
            t = i / max(record.tick_steps, 1)
            radius = initial_radius + (final_radius - initial_radius) * t
            angle = turns * 2 * np.pi * t
            
            a = center_angle + int(radius * np.cos(angle))
            l = center_length + int(radius * np.sin(angle))
            
            path.append(BeamCoordinate(a % 360, l, record.entry.step + i))
        
        return path
    
    def _reconstruct_bounce(self, record: CompressedRecord, blueprint: Blueprint) -> List[BeamCoordinate]:
        """Reconstruct bounce trajectory"""
        bounce_count = blueprint.parameters.get('bounce_count', 2)
        
        # Create bounces by alternating direction
        path = []
        current_angle = record.entry.angle
        current_length = record.entry.length
        
        segment_length = record.tick_steps // max(bounce_count + 1, 1)
        direction = 1
        
        for i in range(record.tick_steps + 1):
            path.append(BeamCoordinate(current_angle, current_length, record.entry.step + i))
            
            # Change direction at bounce points
            if i > 0 and i % segment_length == 0:
                direction *= -1
            
            # Move
            current_length += direction * int((record.settle.length - record.entry.length) / record.tick_steps)
            current_angle += int((record.settle.angle - record.entry.angle) / record.tick_steps)
        
        return path
    
    def calculate_compression_ratio(self, trajectories: List[Trajectory]) -> float:
        """Calculate compression ratio achieved"""
        # Original size: each trajectory stores all points
        original_size = sum(len(t.path) * 6 for t in trajectories)  # 6 bytes per point
        
        # Compressed size: entry + settle + blueprint_id + tick_steps
        compressed_size = len(trajectories) * (6 * 2 + 4 + 4)  # 2 coords + id + steps
        
        return original_size / max(compressed_size, 1)


# ============================================================================
# DEMO
# ============================================================================

def create_test_trajectories(num_trajectories: int = 100) -> List[Trajectory]:
    """Create test trajectories with various patterns"""
    print(f"\n[Demo] Creating {num_trajectories} test trajectories...")
    
    trajectories = []
    
    for i in range(num_trajectories):
        # Random entry point
        entry = BeamCoordinate(
            angle=np.random.randint(0, 360),
            length=np.random.randint(0, 10),
            step=0
        )
        
        # Choose pattern type
        pattern = np.random.choice(['linear', 'angular', 'gravity', 'spiral', 'bounce'])
        
        if pattern == 'linear':
            # Linear movement
            settle = BeamCoordinate(
                angle=entry.angle + np.random.randint(-50, 50),
                length=entry.length + np.random.randint(1, 10),
                step=np.random.randint(5, 15)
            )
        elif pattern == 'angular':
            # Angular movement (rotation)
            radius = np.random.randint(3, 8)
            settle = BeamCoordinate(
                angle=(entry.angle + 90) % 360,
                length=entry.length + radius,
                step=np.random.randint(8, 12)
            )
        elif pattern == 'gravity':
            # Gravity (accelerating downward)
            settle = BeamCoordinate(
                angle=entry.angle + np.random.randint(-10, 10),
                length=entry.length + np.random.randint(10, 20),
                step=np.random.randint(5, 10)
            )
        elif pattern == 'spiral':
            # Spiral movement
            settle = BeamCoordinate(
                angle=(entry.angle + 180) % 360,
                length=entry.length + np.random.randint(5, 15),
                step=np.random.randint(15, 25)
            )
        else:  # bounce
            # Bounce movement
            settle = BeamCoordinate(
                angle=entry.angle + np.random.randint(-30, 30),
                length=entry.length + np.random.randint(3, 8),
                step=np.random.randint(10, 20)
            )
        
        # Generate path based on pattern
        path = []
        steps = settle.step - entry.step
        
        for s in range(steps + 1):
            t = s / max(steps, 1)
            
            if pattern == 'linear':
                a = int(entry.angle + (settle.angle - entry.angle) * t)
                l = int(entry.length + (settle.length - entry.length) * t)
            elif pattern == 'angular':
                angle = t * np.pi / 2
                a = int(entry.angle + radius * np.cos(angle))
                l = int(entry.length + radius * np.sin(angle))
            elif pattern == 'gravity':
                a = int(entry.angle + (settle.angle - entry.angle) * t)
                l = int(entry.length + (settle.length - entry.length) * t * t)  # Quadratic
            elif pattern == 'spiral':
                angle = t * 2 * np.pi
                r = 5 + 10 * t
                a = int(entry.angle + r * np.cos(angle))
                l = int(entry.length + r * np.sin(angle))
            else:  # bounce
                a = int(entry.angle + (settle.angle - entry.angle) * t)
                l = int(entry.length + abs(np.sin(t * np.pi * 2)) * 5)
            
            path.append(BeamCoordinate(a % 360, max(0, l), entry.step + s))
        
        trajectory = Trajectory(
            entry=entry,
            settle=settle,
            path=path,
            tick_steps=steps,
            value=float(i),
            trajectory_id=i
        )
        trajectories.append(trajectory)
    
    print(f"[Demo] Created {len(trajectories)} trajectories")
    return trajectories


def demo_blueprint_system():
    """Demonstrate the full blueprint system"""
    print("\n" + "="*60)
    print("OBSERVATION FIELD + BLUEPRINT SYSTEM DEMO")
    print("="*60)
    
    # Create observation field
    field = ObservationField()
    
    # Create test trajectories
    trajectories = create_test_trajectories(100)
    
    # Feed trajectories to field (simulate observation)
    print("\n[Demo] Feeding trajectories to observation field...")
    for traj in trajectories:
        # Record entry
        field.record_entry(traj.value, traj.entry)
        
        # Record movements (skip first and last)
        for coord in traj.path[1:-1]:
            field.record_movement(traj.value, coord)
        
        # Record settle
        field.record_settle(traj.value, traj.settle)
    
    # Print field stats
    field.print_stats()
    
    # Build blueprints
    engine = BlueprintEngine()
    engine.build_blueprints(field.get_trajectories())
    
    # Print blueprints
    engine.print_blueprints()
    
    # Compress trajectories
    print("\n[Demo] Compressing trajectories...")
    compressed = []
    for traj in trajectories:
        record = engine.compress_trajectory(traj)
        compressed.append(record)
    
    print(f"[Demo] Compressed {len(compressed)} trajectories")
    
    # Demo reconstruction
    print("\n[Demo] Reconstructing trajectories...")
    reconstructor = Reconstructor(engine)
    
    # Reconstruct first 5 trajectories
    for i in range(min(5, len(compressed))):
        original = trajectories[i]
        reconstructed = reconstructor.reconstruct(compressed[i])
        
        # Calculate error
        original_settle = original.settle.to_tuple()
        reconstructed_settle = reconstructed[-1].to_tuple()
        
        error = np.sqrt(sum((o - r)**2 for o, r in zip(original_settle, reconstructed_settle)))
        
        print(f"\n  Trajectory {i}:")
        print(f"    Original:    {original.entry.to_tuple()} → {original.settle.to_tuple()}")
        print(f"    Reconstructed: {reconstructed[0].to_tuple()} → {reconstructed[-1].to_tuple()}")
        print(f"    Settle error: {error:.2f}")
    
    # Calculate compression ratio
    ratio = reconstructor.calculate_compression_ratio(trajectories)
    print(f"\n[Demo] Compression ratio: {ratio:.2f}x")
    
    print("\n" + "="*60)
    print("DEMO COMPLETE")
    print("="*60)


if __name__ == "__main__":
    demo_blueprint_system()
