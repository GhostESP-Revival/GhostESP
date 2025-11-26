#!/usr/bin/env python3
import os
import re
from collections import defaultdict
from pathlib import Path

def parse_ir_file(file_path):
    """Parse a single .ir file and extract POWER button data."""
    power_buttons = []
    
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
    except UnicodeDecodeError:
        try:
            with open(file_path, 'r', encoding='latin-1') as f:
                content = f.read()
        except:
            return power_buttons
    
    sections = content.split('#')
    
    for section in sections:
        section = section.strip()
        if not section:
            continue
            
        name_match = re.search(r'name:\s*(.+)', section, re.IGNORECASE)
        if not name_match:
            continue
            
        name = name_match.group(1).strip()
        
        if name.upper() == 'POWER':
            button_data = {'name': 'POWER', 'source_file': str(file_path)}
            
            type_match = re.search(r'type:\s*(.+)', section, re.IGNORECASE)
            if type_match:
                button_data['type'] = type_match.group(1).strip()
            
            if button_data.get('type') == 'parsed':
                protocol_match = re.search(r'protocol:\s*(.+)', section, re.IGNORECASE)
                if protocol_match:
                    button_data['protocol'] = protocol_match.group(1).strip()
                
                address_match = re.search(r'address:\s*(.+)', section, re.IGNORECASE)
                if address_match:
                    button_data['address'] = address_match.group(1).strip()
                
                command_match = re.search(r'command:\s*(.+)', section, re.IGNORECASE)
                if command_match:
                    button_data['command'] = command_match.group(1).strip()
            
            elif button_data.get('type') == 'raw':
                freq_match = re.search(r'frequency:\s*(.+)', section, re.IGNORECASE)
                if freq_match:
                    button_data['frequency'] = freq_match.group(1).strip()
                
                duty_match = re.search(r'duty_cycle:\s*(.+)', section, re.IGNORECASE)
                if duty_match:
                    button_data['duty_cycle'] = duty_match.group(1).strip()
                
                data_match = re.search(r'data:\s*(.+)', section, re.IGNORECASE)
                if data_match:
                    button_data['data'] = data_match.group(1).strip()
            
            power_buttons.append(button_data)
    
    return power_buttons

def _make_button_key(button):
    """Build a normalized key describing a POWER button variant."""
    button_type = button.get('type', '').strip().lower()
    if button_type == 'parsed':
        return (
            button_type,
            button.get('protocol', '').strip().lower(),
            button.get('address', '').strip().lower(),
            button.get('command', '').strip().lower(),
        )
    return (
        button_type,
        button.get('frequency', '').strip(),
        button.get('duty_cycle', '').strip(),
        button.get('data', '').strip(),
    )

def deduplicate_buttons(buttons):
    """Aggregate duplicate buttons and count how often each variant appears."""
    aggregates = {}

    for button in buttons:
        key = _make_button_key(button)
        aggregate = aggregates.get(key)
        if aggregate is None:
            button_copy = button.copy()
            aggregate = {
                'button': button_copy,
                'count': 0,
                'sources': set(),
            }
            aggregates[key] = aggregate

        aggregate['count'] += 1
        source_file = button.get('source_file')
        if source_file:
            aggregate['sources'].add(source_file)

    unique_buttons = []
    for aggregate in aggregates.values():
        button_entry = aggregate['button']
        button_entry['count'] = aggregate['count']
        if aggregate['sources']:
            button_entry['source_files'] = sorted(aggregate['sources'])
        unique_buttons.append(button_entry)

    return unique_buttons

def hex_to_int(hex_str):
    """Convert hex string to integer."""
    if not hex_str:
        return 0
    try:
        # Handle space-separated hex values (like "04 00 00 00")
        if ' ' in hex_str:
            parts = hex_str.replace('0x', '').split()
            # Convert to little-endian format
            result = 0
            for i, part in enumerate(parts):
                if part.strip():
                    # Shift each byte to its position in little-endian
                    result |= int(part, 16) << (8 * i)
            return result
        else:
            # Handle single hex value
            clean_hex = hex_str.replace('0x', '')
            return int(clean_hex, 16)
    except ValueError:
        return 0

def update_universal_ir_header(buttons, header_path):
    """Update the universal_ir.h file with new POWER buttons."""
    
    # Only include parsed buttons for the C header (no raw format)
    parsed_buttons = [b for b in buttons if b.get('type') == 'parsed']
    
    # Sort by popularity then protocol/address/command
    def secondary_sort_key(button):
        return (button.get('protocol', ''), button.get('address', ''), button.get('command', ''))
    
    parsed_buttons.sort(key=lambda button: (-button.get('count', 0), secondary_sort_key(button)))
    
    # Read the original file to preserve header and footer
    with open(header_path, 'r', encoding='utf-8') as f:
        original_content = f.read()
    
    # Find the start and end of the array
    array_start = original_content.find('static const universal_ir_signal_t universal_ir_signals[UNIVERSAL_IR_SIGNAL_COUNT] = {')
    array_end = original_content.find('};', array_start) + 2
    
    if array_start == -1 or array_end == 1:
        print("Error: Could not find the universal_ir_signals array in the header file")
        return False
    
    # Generate new array content
    array_lines = []
    for button in parsed_buttons:
        protocol = button.get('protocol', '')
        address = hex_to_int(button.get('address', '0x00'))
        command = hex_to_int(button.get('command', '0x00'))
        
        array_lines.append(f'    {{"POWER", "{protocol}", 0x{address:08X}, 0x{command:08X}}},')
    
    new_array_content = 'static const universal_ir_signal_t universal_ir_signals[UNIVERSAL_IR_SIGNAL_COUNT] = {\n' + '\n'.join(array_lines) + '\n};'
    
    # Update the count
    count_define = f'#define UNIVERSAL_IR_SIGNAL_COUNT {len(parsed_buttons)}'
    updated_content = re.sub(r'#define UNIVERSAL_IR_SIGNAL_COUNT \d+', count_define, original_content)
    
    # Replace the array
    updated_content = updated_content[:array_start] + new_array_content + updated_content[array_end:]
    
    # Write back to file
    with open(header_path, 'w', encoding='utf-8') as f:
        f.write(updated_content)
    
    return True

def main():
    ir_files = list(Path('..').rglob('*.ir'))
    print(f"Found {len(ir_files)} .ir files")
    
    all_power_buttons = []
    for ir_file in ir_files:
        buttons = parse_ir_file(ir_file)
        if buttons:
            all_power_buttons.extend(buttons)
    
    print(f"\nTotal POWER buttons found: {len(all_power_buttons)}")
    
    unique_buttons = deduplicate_buttons(all_power_buttons)
    print(f"Unique POWER buttons after deduplication: {len(unique_buttons)}")
    
    # Update the header file
    header_path = "../include/core/universal_ir.h"
    if update_universal_ir_header(unique_buttons, header_path):
        print(f"\nUpdated {header_path} with {len([b for b in unique_buttons if b.get('type') == 'parsed'])} POWER buttons")
        
        # Print summary
        print("\nSummary of buttons included:")
        protocol_counts = defaultdict(int)
        for button in unique_buttons:
            if button['type'] == 'parsed':
                protocol_counts[button.get('protocol', 'Unknown')] += 1
        
        for protocol, count in sorted(protocol_counts.items()):
            print(f"  {protocol}: {count} buttons")
    else:
        print(f"Failed to update {header_path}")

if __name__ == "__main__":
    main()
