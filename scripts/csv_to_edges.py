"""Convert a Saleae transition CSV channel into integer edge sample indices."""
import argparse,csv,decimal,sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('csv');p.add_argument('--channel',type=int,required=True)
p.add_argument('--sample-rate',type=int,required=True)
a=p.parse_args()
if a.sample_rate<=0:p.error('sample rate must be positive')
with open(a.csv,newline='') as f:
    reader=csv.DictReader(f);name=f'Channel {a.channel}'
    if name not in (reader.fieldnames or []):p.error('channel missing from CSV')
    previous=None
    for row in reader:
        level=row[name]
        if previous is not None and level!=previous:
            print(int((decimal.Decimal(row['Time [s]'])*a.sample_rate).to_integral_value(rounding=decimal.ROUND_HALF_UP)))
        previous=level
