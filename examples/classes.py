# classes, floats, and the ROM stdlib together
# expect: 5.0
# expect: <Vec>
import math
class Vec:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def mag(self):
        return sqrt(self.x*self.x + self.y*self.y)
v = Vec(3, 4)
print(v.mag())
print(v)
