# collatz steps from 27 (peaks at 9232, still fits in 16 bits)
# expect: 111
n=27
s=0
while n!=1:
    if n%2==0: n=n//2
    else: n=3*n+1
    s=s+1
print(s)
