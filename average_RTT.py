import re
from collections import defaultdict

def process_log_file(file_path):
    # Dictionary to store RTT stats for each Node ID
    node_rtt_data = defaultdict(lambda: {'total_rtt': 0, 'count': 0, 'min_rtt': float('inf'), 'max_rtt': float('-inf')})

    try:
        with open(file_path, 'r') as file:
            for line in file:
                # Regex to match Node ID and RTT
                match = re.search(r'Node ID (\d+).*?RTT: (\d+) ms', line)
                if match:
                    node_id = int(match.group(1))
                    rtt = int(match.group(2))

                    # Update RTT stats
                    node_rtt_data[node_id]['total_rtt'] += rtt
                    node_rtt_data[node_id]['count'] += 1
                    if rtt > 0:
                        node_rtt_data[node_id]['min_rtt'] = min(node_rtt_data[node_id]['min_rtt'], rtt)
                    node_rtt_data[node_id]['max_rtt'] = max(node_rtt_data[node_id]['max_rtt'], rtt)

        # Calculate and print RTT stats for each Node ID
        print("RTT Stats per Node ID:")
        for node_id, data in node_rtt_data.items():
            average_rtt = data['total_rtt'] / data['count'] if data['count'] > 0 else 0
            min_rtt = data['min_rtt'] if data['min_rtt'] != float('inf') else 0
            max_rtt = data['max_rtt'] if data['max_rtt'] != float('-inf') else 0
            print(f"Node ID {node_id}: minRTT={min_rtt} ms / aveRTT={average_rtt:.2f} ms / maxRTT={max_rtt} ms")

    except FileNotFoundError:
        print(f"Error: File not found: {file_path}")
    except Exception as e:
        print(f"An error occurred: {e}")

# Example usage
if __name__ == "__main__":
    file_path = "C:/Users/ACER/putty.log"
    process_log_file(file_path)
